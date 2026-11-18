#define _GNU_SOURCE
#include "declaration/parser.h"
#include "declaration/ir_dump.h"
#include "declaration/sql.h"
#include "declaration/routes.h"
#include "declaration/workflow.h"
#include "slice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum read_source_tag { READ_SOURCE_OK, READ_SOURCE_IO_ERROR, READ_SOURCE_NO_SPACE, READ_SOURCE_TOO_LARGE, READ_SOURCE_INVALID_PATH } read_source_tag;
typedef struct read_source_result { read_source_tag tag; byte_slice bytes; } read_source_result;
/* stdio adapter: convert the path at the API boundary; returned bytes are owned. */
static read_source_result ReadSource(string_slice path)
{
    if ((!path.ptr && path.len) || path.len == SIZE_MAX) return (read_source_result){.tag = READ_SOURCE_INVALID_PATH};
    SliceFor(path, i) { if (!path.ptr[i]) return (read_source_result){.tag = READ_SOURCE_INVALID_PATH}; }
    byte_slice name = {malloc(path.len + 1), path.len + 1};
    if (!name.ptr) return (read_source_result){.tag = READ_SOURCE_NO_SPACE};
    if (ByteSliceCopy(name, 0, (const_byte_slice){(const u8 *)path.ptr, path.len}).tag != BYTE_SLICE_COPY_OK) {
        free(name.ptr); return (read_source_result){.tag = READ_SOURCE_INVALID_PATH};
    }
    name.ptr[path.len] = 0;
    FILE *file = fopen((const char *)name.ptr, "rb");
    free(name.ptr);
    if (!file) return (read_source_result){.tag = READ_SOURCE_IO_ERROR};
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return (read_source_result){.tag = READ_SOURCE_IO_ERROR}; }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return (read_source_result){.tag = READ_SOURCE_IO_ERROR}; }
    if ((u64)length > UINT32_MAX || (u64)length > SIZE_MAX) { fclose(file); return (read_source_result){.tag = READ_SOURCE_TOO_LARGE}; }
    byte_slice source = {length ? malloc((usize)length) : NULL, (usize)length};
    if (source.len && !source.ptr) { fclose(file); return (read_source_result){.tag = READ_SOURCE_NO_SPACE}; }
    bool read_ok = !source.len || fread(source.ptr, 1, source.len, file) == source.len;
    bool close_ok = fclose(file) == 0;
    if (!read_ok || !close_ok) { free(source.ptr); return (read_source_result){.tag = READ_SOURCE_IO_ERROR}; }
    return (read_source_result){.tag = READ_SOURCE_OK, .bytes = source};
}
typedef struct walk_entry { u32 node; usize depth; } walk_entry;
typedef struct walk_slice { walk_entry *ptr; usize len; } walk_slice;
typedef enum print_ast_tag { PRINT_AST_OK, PRINT_AST_NO_SPACE, PRINT_AST_IO_ERROR, PRINT_AST_INVALID } print_ast_tag;
typedef struct print_ast_result { print_ast_tag tag; } print_ast_result;
/* Iterative preorder walk; tree depth does not consume the C call stack. */
static print_ast_result PrintAst(const declaration_ast *ast, FILE *stream)
{
    walk_slice stack = {calloc(ast->node_tags.len, sizeof(walk_entry)), ast->node_tags.len};
    if (!stack.ptr) return (print_ast_result){PRINT_AST_NO_SPACE};
    usize used = 1;
    stack.ptr[0] = (walk_entry){0, 0};
    print_ast_result result = {PRINT_AST_OK};
    while (used) {
        walk_entry entry = stack.ptr[--used];
        if (entry.node >= ast->node_tags.len || entry.node >= ast->main_tokens.len || entry.node >= ast->node_data.len) { result.tag = PRINT_AST_INVALID; break; }
        u32 token = ast->main_tokens.ptr[entry.node];
        if (token >= ast->token_starts.len || token >= ast->token_ends.len) { result.tag = PRINT_AST_INVALID; break; }
        u32 start = ast->token_starts.ptr[token], end = ast->token_ends.ptr[token];
        const_byte_slice text;
        if (end < start || ConstByteSliceSubslice(ast->source, start, end - start, &text).tag != CONST_BYTE_SLICE_SUBSLICE_OK) { result.tag = PRINT_AST_INVALID; break; }
        string_slice name = DeclarationNodeName((declaration_node_tag)ast->node_tags.ptr[entry.node]);
        for (usize i = 0; i < entry.depth; ++i) { if (fwrite("  ", 1, 2, stream) != 2) { result.tag = PRINT_AST_IO_ERROR; break; } }
        if (result.tag != PRINT_AST_OK) break;
        if (fprintf(stream, "[%u] ", entry.node) < 0 || fwrite(name.ptr, 1, name.len, stream) != name.len ||
            fprintf(stream, " @%u..%u", start, end) < 0) { result.tag = PRINT_AST_IO_ERROR; break; }
        if (entry.node && text.len && (fwrite(" ", 1, 1, stream) != 1 || fwrite(text.ptr, 1, text.len, stream) != text.len)) { result.tag = PRINT_AST_IO_ERROR; break; }
        if (fputc('\n', stream) == EOF) { result.tag = PRINT_AST_IO_ERROR; break; }
        declaration_node_data data = ast->node_data.ptr[entry.node];
        if (data.start > ast->extra.len || data.count > ast->extra.len - data.start || data.count > stack.len - used) { result.tag = PRINT_AST_INVALID; break; }
        declaration_u32_slice children = {data.count ? ast->extra.ptr + data.start : NULL, data.count};
        SliceFor(children, i) { stack.ptr[used++] = (walk_entry){children.ptr[children.len - 1 - i], entry.depth + 1}; }
    }
    free(stack.ptr);
    if (fflush(stream) != 0) result.tag = PRINT_AST_IO_ERROR;
    return result;
}

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>

static volatile sig_atomic_t interrupted;
static void Interrupt(int signal_number) { (void)signal_number; interrupted = 1; }

/* Paths are slices internally. NativePath is the small POSIX adapter: a bounded
 * null-terminated array exists only for calls that require it. */
typedef enum native_path_tag { NATIVE_PATH_OK, NATIVE_PATH_INVALID } native_path_tag;
typedef struct native_path_result { native_path_tag tag; char storage[512]; } native_path_result;
static native_path_result NativePath(string_slice directory, string_slice name)
{
    native_path_result result = {.tag = NATIVE_PATH_INVALID};
    if ((!directory.ptr && directory.len) || (!name.ptr && name.len)) return result;
    SliceFor(directory, i) { if (!directory.ptr[i]) return result; }
    SliceFor(name, i) { if (!name.ptr[i]) return result; }
    usize separator = directory.len && name.len ? 1 : 0;
    if (directory.len >= sizeof(result.storage) || name.len >= sizeof(result.storage) - directory.len - separator) return result;
    byte_slice target = {(u8 *)result.storage, sizeof(result.storage)};
    if (ByteSliceCopy(target, 0, (const_byte_slice){(const u8 *)directory.ptr, directory.len}).tag != BYTE_SLICE_COPY_OK) return result;
    if (separator) result.storage[directory.len] = '/';
    if (ByteSliceCopy(target, directory.len + separator, (const_byte_slice){(const u8 *)name.ptr, name.len}).tag != BYTE_SLICE_COPY_OK) return result;
    result.storage[directory.len + separator + name.len] = 0;
    result.tag = NATIVE_PATH_OK;
    return result;
}
static string_slice NativeSlice(const native_path_result *path)
{ return (string_slice){path->storage, strlen(path->storage)}; }
static bool SameBytes(const_byte_slice a, const_byte_slice b)
{
    if (a.len != b.len) return false;
    SliceFor(a, i) { if (a.ptr[i] != b.ptr[i]) return false; }
    return true;
}
static void Diagnostic(string_slice path, const_byte_slice source, u32 offset, string_slice message)
{
    usize line = 1, column = 1;
    SliceFor(source, i) { if (i >= offset) break; if (source.ptr[i] == '\n') { ++line; column = 1; } else ++column; }
    fwrite(path.ptr, 1, path.len, stderr); fprintf(stderr, ":%zu:%zu: ", line, column);
    fwrite(message.ptr, 1, message.len, stderr); fputc('\n', stderr);
}
typedef enum compile_tag { COMPILE_OK, COMPILE_FAILED } compile_tag;
typedef struct compile_result { compile_tag tag; } compile_result;
static compile_result Compile(string_slice path, const_byte_slice source, declaration_ast *ast, declaration_ir *ir)
{
    declaration_parse_result parsed = DeclarationParse(source, ast);
    if (parsed.tag != DECLARATION_PARSE_OK) { Diagnostic(path, source, parsed.offset, parsed.message); return (compile_result){COMPILE_FAILED}; }
    declaration_resolve_result resolved = DeclarationResolve(ast, ir);
    if (resolved.tag != DECLARATION_RESOLVE_OK) { Diagnostic(path, source, resolved.offset, resolved.message); return (compile_result){COMPILE_FAILED}; }
    return (compile_result){COMPILE_OK};
}
typedef enum ensure_directory_tag { ENSURE_DIRECTORY_OK, ENSURE_DIRECTORY_FAILED } ensure_directory_tag;
typedef struct ensure_directory_result { ensure_directory_tag tag; } ensure_directory_result;
static ensure_directory_result EnsureDirectory(string_slice directory)
{
    native_path_result path = NativePath(directory, (string_slice){0});
    if (path.tag != NATIVE_PATH_OK) return (ensure_directory_result){ENSURE_DIRECTORY_FAILED};
    if (mkdir(path.storage, 0755) != 0 && errno != EEXIST) return (ensure_directory_result){ENSURE_DIRECTORY_FAILED};
    struct stat info;
    return (ensure_directory_result){lstat(path.storage, &info) == 0 && S_ISDIR(info.st_mode) ? ENSURE_DIRECTORY_OK : ENSURE_DIRECTORY_FAILED};
}
typedef enum history_tag { HISTORY_OK, HISTORY_FAILED } history_tag;
typedef struct history_result { history_tag tag; u32 latest; } history_result;
static history_result History(void)
{
    history_result failure = {.tag = HISTORY_FAILED};
    i32 descriptor = open("versions", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) return errno == ENOENT ? (history_result){.tag = HISTORY_OK} : failure;
    DIR *directory = fdopendir(descriptor);
    if (!directory) { close(descriptor); return failure; }
    u32 count = 0, latest = 0;
    bool valid = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { if (errno) valid = false; break; }
        string_slice name = {entry->d_name, strlen(entry->d_name)};
        if (name.len && name.ptr[0] == '.') continue; /* Includes unpublished staging directories. */
        u32 version = 0;
        SliceFor(name, i) {
            u8 c = (u8)name.ptr[i];
            if (c < '0' || c > '9' || version > (UINT32_MAX - (c - '0')) / 10) { valid = false; break; }
            version = version * 10 + c - '0';
        }
        char canonical[16];
        i32 length = snprintf(canonical, sizeof(canonical), "%04u", version);
        if (!valid || !version || length < 0 || (usize)length >= sizeof(canonical) || !SameBytes((const_byte_slice){(const u8 *)name.ptr, name.len}, (const_byte_slice){(const u8 *)canonical, (usize)length})) { valid = false; break; }
        i32 snapshot = openat(descriptor, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (snapshot < 0) { valid = false; break; }
        struct stat cfg, sql, schema;
        bool complete = fstatat(snapshot, "app.cfg", &cfg, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(cfg.st_mode) &&
            fstatat(snapshot, "migration.sql", &sql, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(sql.st_mode) &&
            fstatat(snapshot, "schema", &schema, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(schema.st_mode);
        close(snapshot);
        if (!complete || count == UINT32_MAX) { valid = false; break; }
        ++count; if (version > latest) latest = version;
    }
    if (closedir(directory) != 0) valid = false;
    if (!valid || count != latest) return failure;
    return (history_result){.tag = HISTORY_OK, .latest = latest};
}
typedef enum finish_file_tag { FINISH_FILE_OK, FINISH_FILE_FAILED } finish_file_tag;
typedef struct finish_file_result { finish_file_tag tag; } finish_file_result;
static finish_file_result FinishFile(FILE *file)
{
    bool okay = fflush(file) == 0;
    if (okay && fsync(fileno(file)) != 0) okay = false;
    if (fclose(file) != 0) okay = false;
    return (finish_file_result){okay ? FINISH_FILE_OK : FINISH_FILE_FAILED};
}
typedef enum artifact_kind { ARTIFACT_AST, ARTIFACT_IR, ARTIFACT_SQL, ARTIFACT_SCHEMA, ARTIFACT_ROUTES_H, ARTIFACT_ROUTES_C } artifact_kind;
typedef enum emit_tag { EMIT_OK, EMIT_FAILED } emit_tag;
typedef struct emit_result { emit_tag tag; } emit_result;
static emit_result Emit(string_slice stage, string_slice name, artifact_kind kind, const declaration_ast *ast, const declaration_ir *ir, const physical_schema *desired, const physical_schema *previous, physical_schema *actual)
{
    native_path_result path = NativePath(stage, name);
    if (path.tag != NATIVE_PATH_OK) return (emit_result){EMIT_FAILED};
    FILE *file = fopen(path.storage, "wbx");
    if (!file) return (emit_result){EMIT_FAILED};
    bool okay = false;
    switch (kind) {
    case ARTIFACT_ROUTES_H:
    case ARTIFACT_ROUTES_C: {
        declaration_routes_result routes = DeclarationRoutes(ir, file, kind == ARTIFACT_ROUTES_H);
        okay = routes.tag == DECLARATION_ROUTES_OK;
        if (!okay) {
            fprintf(stderr, "Routes at bytes %u and %u: ", routes.offset, routes.other_offset);
            fwrite(routes.message.ptr, 1, routes.message.len, stderr); fputc('\n', stderr);
        }
        break;
    }
    case ARTIFACT_AST: okay = PrintAst(ast, file).tag == PRINT_AST_OK; break;
    case ARTIFACT_IR: okay = DeclarationIrDump(ir, file).tag == DECLARATION_IR_DUMP_OK; break;
    case ARTIFACT_SCHEMA: okay = PhysicalWrite(actual, file).tag == PHYSICAL_WRITE_OK; break;
    case ARTIFACT_SQL: {
        physical_diff_result generated = PhysicalDiff(desired, previous, actual, file);
        okay = generated.tag == PHYSICAL_DIFF_OK;
        if (!okay) { fwrite(generated.message.ptr, 1, generated.message.len, stderr); fputc('\n', stderr); }
        break;
    }
    }
    if (FinishFile(file).tag != FINISH_FILE_OK) okay = false;
    return (emit_result){okay ? EMIT_OK : EMIT_FAILED};
}
typedef enum write_snapshot_tag { WRITE_SNAPSHOT_OK, WRITE_SNAPSHOT_FAILED } write_snapshot_tag;
typedef struct write_snapshot_result { write_snapshot_tag tag; } write_snapshot_result;
static write_snapshot_result WriteSnapshot(string_slice stage, string_slice name, const_byte_slice bytes)
{
    native_path_result path = NativePath(stage, name);
    if (path.tag != NATIVE_PATH_OK) return (write_snapshot_result){WRITE_SNAPSHOT_FAILED};
    FILE *file = fopen(path.storage, "wbx");
    if (!file) return (write_snapshot_result){WRITE_SNAPSHOT_FAILED};
    bool okay = !bytes.len || fwrite(bytes.ptr, 1, bytes.len, file) == bytes.len;
    if (FinishFile(file).tag != FINISH_FILE_OK) okay = false;
    return (write_snapshot_result){okay ? WRITE_SNAPSHOT_OK : WRITE_SNAPSHOT_FAILED};
}
typedef enum sync_directory_tag { SYNC_DIRECTORY_OK, SYNC_DIRECTORY_FAILED } sync_directory_tag;
typedef struct sync_directory_result { sync_directory_tag tag; } sync_directory_result;
static sync_directory_result SyncDirectory(string_slice directory)
{
    native_path_result path = NativePath(directory, (string_slice){0});
    if (path.tag != NATIVE_PATH_OK) return (sync_directory_result){SYNC_DIRECTORY_FAILED};
    i32 fd = open(path.storage, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return (sync_directory_result){SYNC_DIRECTORY_FAILED};
    bool okay = fsync(fd) == 0;
    if (close(fd) != 0) okay = false;
    return (sync_directory_result){okay ? SYNC_DIRECTORY_OK : SYNC_DIRECTORY_FAILED};
}
typedef enum move_file_tag { MOVE_FILE_OK, MOVE_FILE_FAILED } move_file_tag;
typedef struct move_file_result { move_file_tag tag; } move_file_result;
static move_file_result MoveFile(string_slice from, string_slice to, string_slice name)
{
    native_path_result source = NativePath(from, name), destination = NativePath(to, name);
    if (source.tag != NATIVE_PATH_OK || destination.tag != NATIVE_PATH_OK) return (move_file_result){MOVE_FILE_FAILED};
    return (move_file_result){rename(source.storage, destination.storage) == 0 ? MOVE_FILE_OK : MOVE_FILE_FAILED};
}
typedef enum cleanup_tag { CLEANUP_OK, CLEANUP_FAILED } cleanup_tag;
typedef enum publish_generated_tag { PUBLISH_GENERATED_OK, PUBLISH_GENERATED_FAILED } publish_generated_tag;
typedef struct publish_generated_result { publish_generated_tag tag; } publish_generated_result;
static publish_generated_result PublishGenerated(string_slice stage)
{
    native_path_result path = NativePath(stage, (string_slice){0});
    if (path.tag != NATIVE_PATH_OK) return (publish_generated_result){PUBLISH_GENERATED_FAILED};
    DIR *entries = opendir(path.storage);
    if (!entries) return (publish_generated_result){PUBLISH_GENERATED_FAILED};
    bool okay = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(entries);
        if (!entry) { if (errno) okay = false; break; }
        if (entry->d_name[0] == '.') continue;
        string_slice name = {entry->d_name, strlen(entry->d_name)};
        if (MoveFile(stage, StringSlice("src/generated"), name).tag != MOVE_FILE_OK) { okay = false; break; }
    }
    if (closedir(entries) != 0) okay = false;
    return (publish_generated_result){okay ? PUBLISH_GENERATED_OK : PUBLISH_GENERATED_FAILED};
}
typedef struct cleanup_result { cleanup_tag tag; } cleanup_result;
static cleanup_result Cleanup(string_slice directory)
{
    native_path_result path = NativePath(directory, (string_slice){0});
    if (path.tag != NATIVE_PATH_OK) return (cleanup_result){CLEANUP_FAILED};
    DIR *entries = opendir(path.storage);
    if (!entries) return (cleanup_result){CLEANUP_FAILED};
    bool okay = true;
    /* Only task-owned staging directories are accepted by callers. */
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(entries);
        if (!entry) { if (errno) okay = false; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (unlinkat(dirfd(entries), entry->d_name, 0) != 0) okay = false;
    }
    if (closedir(entries) != 0 || rmdir(path.storage) != 0) okay = false;
    return (cleanup_result){okay ? CLEANUP_OK : CLEANUP_FAILED};
}
int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3 || (strcmp(argv[1], "build") && strcmp(argv[1], "version"))) {
        fputs("Usage: app-compiler build|version [project-directory]\n", stderr); return 2;
    }
    bool record = strcmp(argv[1], "version") == 0;
    struct sigaction action = {.sa_handler = Interrupt};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) {
        fputs("Cannot install cleanup signal handlers\n", stderr); return 1;
    }
    if (argc == 3 && chdir(argv[2]) != 0) { fputs("Cannot open project directory\n", stderr); return 1; }
    if (EnsureDirectory(StringSlice("build")).tag != ENSURE_DIRECTORY_OK) { fputs("Cannot create build directory\n", stderr); return 1; }
    i32 lock = open("build/.app-compiler.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX) != 0) { if (lock >= 0) close(lock); fputs("Cannot lock project build\n", stderr); return 1; }
    i32 status = 1;
    byte_slice source = {0}, prior_source = {0}, migration = {0}, schema_bytes = {0};
    declaration_ast ast = {0};
    declaration_ir ir = {0};
    physical_schema desired = {0}, previous = {0}, actual = {0};
    char build_template[] = "build/.compile-XXXXXX", version_template[] = "versions/.pending-XXXXXX";
    char generated_template[] = "src/generated/.compile-XXXXXX";
    string_slice build_stage = {0}, version_stage = {0}, generated_stage = {0};
    read_source_result read = ReadSource(StringSlice("app.cfg"));
    if (read.tag != READ_SOURCE_OK) { fputs("Cannot read app.cfg\n", stderr); goto done; }
    source = read.bytes;
    if (Compile(StringSlice("app.cfg"), (const_byte_slice){source.ptr, source.len}, &ast, &ir).tag != COMPILE_OK) goto done;
    if (interrupted) goto done;
    history_result history = History();
    if (history.tag != HISTORY_OK) { fputs("Invalid versions history: require contiguous numbered directories with app.cfg, schema, and migration.sql; legacy snapshots require an explicit upgrade\n", stderr); goto done; }
    if ((!history.latest && ir.version != 1) || (history.latest && ir.version != history.latest && (history.latest == UINT32_MAX || ir.version != history.latest + 1))) {
        fputs("Version must equal the latest recorded version or its successor (starting at 1)\n", stderr); goto done;
    }
    if (history.latest) {
        char prior_directory[32];
        i32 length = snprintf(prior_directory, sizeof(prior_directory), "versions/%04u", history.latest);
        if (length < 0 || (usize)length >= sizeof(prior_directory)) goto done;
        native_path_result path = NativePath((string_slice){prior_directory, (usize)length}, StringSlice("app.cfg"));
        if (path.tag != NATIVE_PATH_OK) goto done;
        read_source_result prior = ReadSource(NativeSlice(&path));
        if (prior.tag != READ_SOURCE_OK) { fputs("Cannot read latest version snapshot\n", stderr); goto done; }
        prior_source = prior.bytes;
        path = NativePath((string_slice){prior_directory, (usize)length}, StringSlice("schema"));
        if (path.tag != NATIVE_PATH_OK) goto done;
        read_source_result prior_schema = ReadSource(NativeSlice(&path));
        if (prior_schema.tag != READ_SOURCE_OK) { fputs("Cannot read recorded physical schema\n", stderr); goto done; }
        physical_read_result loaded = PhysicalRead((const_byte_slice){prior_schema.bytes.ptr, prior_schema.bytes.len}, &previous);
        free(prior_schema.bytes.ptr);
        if (loaded.tag != PHYSICAL_READ_OK || previous.version != history.latest) { fputs("Invalid recorded physical schema or snapshot version\n", stderr); goto done; }
        if (record && ir.version == history.latest && !SameBytes((const_byte_slice){source.ptr, source.len}, (const_byte_slice){prior_source.ptr, prior_source.len})) {
            fputs("Recorded versions are immutable; increment version() before recording changes\n", stderr); goto done;
        }
    }
    physical_build_result physical = PhysicalBuild(&ir, &desired);
    if (physical.tag != PHYSICAL_BUILD_OK) {
        fwrite(physical.message.ptr, 1, physical.message.len, stderr); fputc('\n', stderr); goto done;
    }
    if (!mkdtemp(build_template)) { fputs("Cannot create build staging directory\n", stderr); goto done; }
    build_stage = (string_slice){build_template, strlen(build_template)};
    if (EnsureDirectory(StringSlice("src")).tag != ENSURE_DIRECTORY_OK ||
        EnsureDirectory(StringSlice("src/generated")).tag != ENSURE_DIRECTORY_OK || !mkdtemp(generated_template)) {
        fputs("Cannot create generated-source staging directory\n", stderr); goto done;
    }
    generated_stage = (string_slice){generated_template, strlen(generated_template)};
    if (Emit(build_stage, StringSlice("app-ast.txt"), ARTIFACT_AST, &ast, &ir, NULL, NULL, NULL).tag != EMIT_OK ||
        Emit(generated_stage, StringSlice("app-routes.h"), ARTIFACT_ROUTES_H, &ast, &ir, NULL, NULL, NULL).tag != EMIT_OK ||
        Emit(generated_stage, StringSlice("app-routes.c"), ARTIFACT_ROUTES_C, &ast, &ir, NULL, NULL, NULL).tag != EMIT_OK ||
        Emit(build_stage, StringSlice("app-ir.txt"), ARTIFACT_IR, &ast, &ir, NULL, NULL, NULL).tag != EMIT_OK ||
        Emit(build_stage, StringSlice("migration.sql"), ARTIFACT_SQL, &ast, &ir, &desired, history.latest ? &previous : NULL, &actual).tag != EMIT_OK ||
        Emit(build_stage, StringSlice("schema"), ARTIFACT_SCHEMA, &ast, &ir, NULL, NULL, &actual).tag != EMIT_OK) {
        fputs("Build failed; previous build outputs and recorded versions were preserved\n", stderr); goto done;
    }
    workflow_generate_result workflow = WorkflowGenerate(&ir, generated_stage);
    if (workflow.tag != WORKFLOW_GENERATE_OK) {
        fwrite(workflow.message.ptr, 1, workflow.message.len, stderr); fputc('\n', stderr); goto done;
    }
    if (record && ir.version != history.latest) {
        if (interrupted) goto done;
        if (EnsureDirectory(StringSlice("versions")).tag != ENSURE_DIRECTORY_OK || !mkdtemp(version_template)) { fputs("Cannot stage recorded version\n", stderr); goto done; }
        version_stage = (string_slice){version_template, strlen(version_template)};
        native_path_result sql_path = NativePath(build_stage, StringSlice("migration.sql"));
        if (sql_path.tag != NATIVE_PATH_OK) goto done;
        read_source_result sql = ReadSource(NativeSlice(&sql_path));
        if (sql.tag != READ_SOURCE_OK) goto done;
        migration = sql.bytes;
        native_path_result schema_path = NativePath(build_stage, StringSlice("schema"));
        if (schema_path.tag != NATIVE_PATH_OK) goto done;
        read_source_result snapshot = ReadSource(NativeSlice(&schema_path));
        if (snapshot.tag != READ_SOURCE_OK) goto done;
        schema_bytes = snapshot.bytes;
        if (WriteSnapshot(version_stage, StringSlice("app.cfg"), (const_byte_slice){source.ptr, source.len}).tag != WRITE_SNAPSHOT_OK ||
            WriteSnapshot(version_stage, StringSlice("migration.sql"), (const_byte_slice){migration.ptr, migration.len}).tag != WRITE_SNAPSHOT_OK ||
            WriteSnapshot(version_stage, StringSlice("schema"), (const_byte_slice){schema_bytes.ptr, schema_bytes.len}).tag != WRITE_SNAPSHOT_OK ||
            chmod(version_template, 0755) != 0 ||
            SyncDirectory(version_stage).tag != SYNC_DIRECTORY_OK) { fputs("Cannot write version snapshot\n", stderr); goto done; }
        char destination[32];
        if (interrupted) goto done;
        i32 length = snprintf(destination, sizeof(destination), "versions/%04u", ir.version);
        if (length < 0 || (usize)length >= sizeof(destination) || renameat2(AT_FDCWD, version_template, AT_FDCWD, destination, RENAME_NOREPLACE) != 0) {
            fputs("Cannot publish version without overwriting an existing directory\n", stderr); goto done;
        }
        version_stage = (string_slice){0};
        if (SyncDirectory(StringSlice("versions")).tag != SYNC_DIRECTORY_OK || SyncDirectory(StringSlice(".")).tag != SYNC_DIRECTORY_OK) { fputs("Version was published but directory sync failed; inspect history before retrying\n", stderr); goto done; }
    }
    if (interrupted) goto done;
    if (MoveFile(build_stage, StringSlice("build"), StringSlice("app-ast.txt")).tag != MOVE_FILE_OK ||
        PublishGenerated(generated_stage).tag != PUBLISH_GENERATED_OK ||
        MoveFile(build_stage, StringSlice("build"), StringSlice("app-ir.txt")).tag != MOVE_FILE_OK ||
        MoveFile(build_stage, StringSlice("build"), StringSlice("migration.sql")).tag != MOVE_FILE_OK ||
        MoveFile(build_stage, StringSlice("build"), StringSlice("schema")).tag != MOVE_FILE_OK ||
        SyncDirectory(StringSlice("build")).tag != SYNC_DIRECTORY_OK ||
        SyncDirectory(StringSlice("src/generated")).tag != SYNC_DIRECTORY_OK) { fputs("Cannot publish all build outputs; rebuild to refresh them\n", stderr); goto done; }
    printf("%s version %u: build/app-ast.txt, build/app-ir.txt, build/schema, build/migration.sql, src/generated/app-routes.{h,c}\n", record ? "Recorded" : "Built", ir.version);
    if (record) printf("Snapshot: versions/%04u/{app.cfg,schema,migration.sql}%s\n", ir.version, ir.version == history.latest ? " (already recorded; unchanged)" : "");
    status = 0;
done:
    if (generated_stage.len && Cleanup(generated_stage).tag != CLEANUP_OK) { fputs("Could not remove generated-source staging directory\n", stderr); status = 1; }
    if (version_stage.len && Cleanup(version_stage).tag != CLEANUP_OK) { fputs("Could not remove version staging directory\n", stderr); status = 1; }
    if (build_stage.len && Cleanup(build_stage).tag != CLEANUP_OK) { fputs("Could not remove build staging directory\n", stderr); status = 1; }
    DeclarationAstDestroy(&ast);
    DeclarationIrDestroy(&ir);
    PhysicalDestroy(&desired); PhysicalDestroy(&previous); PhysicalDestroy(&actual);
    free(source.ptr); free(prior_source.ptr); free(migration.ptr); free(schema_bytes.ptr);
    close(lock);
    return status;
}
