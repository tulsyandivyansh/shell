#include "include/process_info.hpp"

#include <cassert>
#include <csignal>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
    const auto self = shell::inspect_process(::getpid());
    assert(self.ok());
    assert(self.info->pid == ::getpid());
    assert(self.info->ppid > 0);
    assert(self.info->threads >= 1);
    assert(!self.info->name.empty());
    assert(!self.info->command_line.empty());
    assert(self.info->open_file_descriptors.has_value());

    const auto missing = shell::inspect_process(99999999);
    assert(!missing.ok());

    int ready[2];
    assert(::pipe(ready) == 0);
    const pid_t child = ::fork();
    assert(child >= 0);

    if (child == 0) {
        ::close(ready[0]);
        (void)::prctl(PR_SET_NAME, "a)b c", 0, 0, 0);
        const char marker = 'x';
        (void)::write(ready[1], &marker, 1);
        ::close(ready[1]);
        for (;;) {
            ::pause();
        }
    }

    ::close(ready[1]);
    char marker = 0;
    assert(::read(ready[0], &marker, 1) == 1);
    ::close(ready[0]);

    const auto child_info = shell::inspect_process(child);
    assert(child_info.ok());
    assert(child_info.info->pid == child);
    assert(child_info.info->ppid == ::getpid());
    // The comm field is wrapped in parentheses in /proc/<pid>/stat. This
    // deliberately contains both a ')' and a space to exercise robust parsing.
    assert(child_info.info->name == "a)b c");

    ::kill(child, SIGTERM);
    int status = 0;
    assert(::waitpid(child, &status, 0) == child);
    return 0;
}
