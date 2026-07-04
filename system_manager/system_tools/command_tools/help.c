// ToxenOS/system_manager/system_tools/command_tools/help.c — Milestone
// 16: `help` promoted from a shell64.c builtin to a real standalone
// command, mirroring this codebase's own Milestone 11 precedent (`shw`/
// `stat` were builtins until real external commands replaced them and
// the redundant builtins were deleted -- no special-casing is kept once
// a real command exists). All of shell64.c's Milestone 14 multi-page
// help logic moves here verbatim; shell64.c now resolves `help` through
// the normal resolve_and_spawn path like every other command. `exit`
// and `clear`/`cls` stay real shell64.c builtins -- both inherently
// non-externalizable (exit acts on the shell's own process; clear is
// already a one-line syscall wrapper with nothing left to move out).
//
// Takes its one argument (the help topic, may be empty) via
// sys_get_args exactly like every other command_tools program.
#include <stdint.h>
#include "tox64.h"
#include "toxcolor64.h"

static int my_strlen(const char* s) {
    int i = 0;
    while (s[i]) i++;
    return i;
}

static int str_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

static void put(const char* s) {
    sys_write(s, (uint64_t)my_strlen(s));
}

// Prints `s` in color `c`, then resets to the console's default -- the
// shape every colored segment below uses (headers, command names,
// errors), so the color never bleeds into whatever's printed next.
static void put_colored(uint8_t c, const char* s) {
    sys_set_color(c);
    put(s);
    sys_set_color(TC64_DEFAULT);
}

// Prints one help-table row with the command name colored and padded
// to a fixed column so descriptions line up regardless of name length
// (longest name today is "nexinfo", 7 chars).
static void help_cmd_line(const char* name, const char* desc) {
    put("  ");
    put_colored(TC64_WHITE, name);
    int pad = 8 - my_strlen(name);
    for (int i = 0; i < pad; i++) put(" ");
    put("- "); put(desc); put("\n");
}

// Milestone 14: help is a small set of short pages instead of one
// dense dump -- every line below is kept well under 70 columns on
// purpose, and any path that has to appear gets its own line rather
// than being inlined into a sentence.

static void help_main(void) {
    put_colored(TC64_CYAN_BRIGHT, "ToxenOS64 shell\n");
    put("Navigate: ls  cd  pwd  up\n");
    put("Read:     shw  stat  nexinfo\n");
    put("Write:    mkdir  mkfile  write  del\n");
    put("Organise: ren  copy  move\n");
    put("Other:    where  help  clear  exit\n");
    put("Try: help commands\n");
    put("Try: help cd\n");
}

static void help_commands(void) {
    put_colored(TC64_CYAN_BRIGHT, "Builtins:\n");
    help_cmd_line("cd",    "change directory");
    help_cmd_line("up",    "go to the parent folder");
    help_cmd_line("pwd",   "print current directory");
    help_cmd_line("clear", "clear the screen");
    help_cmd_line("exit",  "leave the shell");
    put_colored(TC64_CYAN_BRIGHT, "Commands:\n");
    help_cmd_line("ls",      "list a directory's contents");
    help_cmd_line("shw",     "show a file's contents");
    help_cmd_line("stat",    "show file size and type");
    help_cmd_line("mkdir",   "create a folder");
    help_cmd_line("mkfile",  "create an empty file");
    help_cmd_line("write",   "write text to a file");
    help_cmd_line("del",     "delete a file or empty folder");
    help_cmd_line("ren",     "rename a file or folder");
    help_cmd_line("copy",    "copy a file");
    help_cmd_line("move",    "move a file or folder");
    help_cmd_line("where",   "show how a command resolves");
    help_cmd_line("nexinfo", "show NEX64/ELF64 file info");
    help_cmd_line("help",    "this help system");
    put("\n");
    put("Try: help <name> for details\n");
}

static void help_paths(void) {
    put("The shell starts in your home folder:\n");
    put("  C:\\System Manager\\User\\\n");
    put("  Profiles\\Default\\\n");
    put("\n");
    put("ToxenOS paths use C:\\ and backslashes.\n");
    put("Quote names that contain spaces:\n");
    put("  cd \"System Manager\"\n");
    put("  cd \"C:\\System Manager\\System Tools\"\n");
    put("\n");
    put("Commands are found automatically in:\n");
    put("  C:\\System Manager\\\n");
    put("  System Tools\\Command Tools\\\n");
    put("\n");
    put("cd alone returns home.\n");
    put("cd C:\\ or cd \\ goes to the root.\n");
    put("Try: help cd\n");
}

static void help_shw(void) {
    put_colored(TC64_YELLOW, "shw - show a file's contents\n");
    put("Usage: shw <file>\n");
    put("Example: shw /hello.ts\n");
}

static void help_stat(void) {
    put_colored(TC64_YELLOW, "stat - show file size and type\n");
    put("Usage: stat <file>\n");
    put("Example: stat /hello.ts\n");
}

static void help_where(void) {
    put_colored(TC64_YELLOW, "where - show how a name resolves\n");
    put("Usage: where <command>\n");
    put("Example: where shw\n");
}

static void help_nexinfo(void) {
    put_colored(TC64_YELLOW, "nexinfo - show NEX64/ELF64 file info\n");
    put("Usage: nexinfo <file>\n");
    put("Example: nexinfo /hello.ts\n");
}

static void help_ls(void) {
    put_colored(TC64_YELLOW, "ls - list a directory's contents\n");
    put("Usage: ls [dir]\n");
    put("With no argument, lists the current\n");
    put("directory.\n");
    put("Example: ls\n");
}

static void help_mkdir(void) {
    put_colored(TC64_YELLOW, "mkdir - create a folder\n");
    put("Usage: mkdir <name>\n");
    put("Example: mkdir Projects\n");
    put("Example: mkdir \"My Folder\"\n");
}

static void help_mkfile(void) {
    put_colored(TC64_YELLOW, "mkfile - create an empty file\n");
    put("Usage: mkfile <name>\n");
    put("Example: mkfile notes.txt\n");
}

static void help_write(void) {
    put_colored(TC64_YELLOW, "write - write text to a file\n");
    put("Usage: write <file> <text>\n");
    put("Replaces the whole file. mkfile first.\n");
    put("Example: write notes.txt Hello ToxenOS\n");
}

static void help_del(void) {
    put_colored(TC64_YELLOW, "del - delete a file or empty folder\n");
    put("Usage: del <path>\n");
    put("Example: del notes.txt\n");
    put("Example: del Projects\n");
    put("Fails if folder is not empty.\n");
}

static void help_ren(void) {
    put_colored(TC64_YELLOW, "ren - rename a file or folder\n");
    put("Usage: ren <old> <new>\n");
    put("Fails if target already exists.\n");
    put("Example: ren notes.txt todo.txt\n");
    put("Example: ren Projects Work\n");
    put("Example: ren \"My File.txt\" \"Better Name.txt\"\n");
}

static void help_copy(void) {
    put_colored(TC64_YELLOW, "copy - copy a file\n");
    put("Usage: copy <source> <dest>\n");
    put("Fails if dest exists. Files only (no dir copy).\n");
    put("Example: copy notes.txt notes_backup.txt\n");
    put("Example: copy \"Documents\\test.txt\" \"Desktop\\test.txt\"\n");
}

static void help_move(void) {
    put_colored(TC64_YELLOW, "move - move a file or folder\n");
    put("Usage: move <source> <dest>\n");
    put("Fails if dest exists.\n");
    put("Example: move notes.txt Documents\\notes.txt\n");
    put("Example: move Projects Documents\\Projects\n");
}

static void help_pwd(void) {
    put_colored(TC64_YELLOW, "pwd - print current directory\n");
    put("Usage: pwd\n");
    put("Prints the current path, e.g.:\n");
    put("  C:\\System Manager\\User\\\n");
    put("  Profiles\\Default\\\n");
}

static void help_cd(void) {
    put_colored(TC64_YELLOW, "cd - change directory\n");
    put("Usage: cd [path]\n");
    put("  cd      go to your home folder\n");
    put("  cd C:\\  go to the drive root\n");
    put("  cd \\    go to the drive root\n");
    put("  cd ..   go to the parent folder\n");
    put("Quote names with spaces, e.g.:\n");
    put("Example: cd \"System Manager\"\n");
}

static void help_up(void) {
    put_colored(TC64_YELLOW, "up - go to the parent folder\n");
    put("Usage: up\n");
    put("Same as: cd ..\n");
}

static void help_exit(void) {
    put_colored(TC64_YELLOW, "exit - leave the shell\n");
    put("Usage: exit\n");
}

static void help_clear(void) {
    put_colored(TC64_YELLOW, "clear - clear the screen\n");
    put("Usage: clear  (alias: cls)\n");
}

static void help_help(void) {
    put_colored(TC64_YELLOW, "help - this help system\n");
    put("Usage: help [topic]\n");
    put("Example: help commands\n");
}

void _start(void) {
    char topic[128];
    int64_t n = sys_get_args(topic, sizeof(topic));
    if (n < 0) topic[0] = 0;

    if (!topic[0])                       { help_main();     sys_exit(0); }
    if (str_eq(topic, "commands"))     { help_commands(); sys_exit(0); }
    if (str_eq(topic, "paths"))        { help_paths();    sys_exit(0); }
    if (str_eq(topic, "shw"))          { help_shw();       sys_exit(0); }
    if (str_eq(topic, "stat"))         { help_stat();      sys_exit(0); }
    if (str_eq(topic, "where"))        { help_where();     sys_exit(0); }
    if (str_eq(topic, "nexinfo"))      { help_nexinfo();   sys_exit(0); }
    if (str_eq(topic, "ls"))           { help_ls();        sys_exit(0); }
    if (str_eq(topic, "pwd"))          { help_pwd();       sys_exit(0); }
    if (str_eq(topic, "mkdir"))        { help_mkdir();     sys_exit(0); }
    if (str_eq(topic, "mkfile"))       { help_mkfile();    sys_exit(0); }
    if (str_eq(topic, "write"))        { help_write();     sys_exit(0); }
    if (str_eq(topic, "del"))          { help_del();       sys_exit(0); }
    if (str_eq(topic, "ren"))          { help_ren();       sys_exit(0); }
    if (str_eq(topic, "copy"))         { help_copy();      sys_exit(0); }
    if (str_eq(topic, "move"))         { help_move();      sys_exit(0); }
    if (str_eq(topic, "cd"))           { help_cd();        sys_exit(0); }
    if (str_eq(topic, "up"))           { help_up();        sys_exit(0); }
    if (str_eq(topic, "help"))         { help_help();      sys_exit(0); }
    if (str_eq(topic, "exit"))         { help_exit();      sys_exit(0); }
    if (str_eq(topic, "clear") || str_eq(topic, "cls")) { help_clear(); sys_exit(0); }

    sys_set_color(TC64_RED_BRIGHT);
    put("help: no help page for '"); put(topic); put("'\n");
    sys_set_color(TC64_DEFAULT);
    put("Try: help commands\n");
    sys_exit(1);

    for (;;) { }  // unreachable
}
