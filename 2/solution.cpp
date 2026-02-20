// solution.cpp
#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>

static int
run_pipeline(const std::vector<const command *> &cmds,
             bool redirect_out, enum output_type out_type, const std::string &out_file,
             bool background)
{
    size_t n = cmds.size();
    if (n == 0) return 0;

    std::vector<int> pipefds;
    pipefds.reserve((n>0 ? n-1 : 0) * 2);
    for (size_t i = 0; i + 1 < n; ++i) {
        int fds[2];
        if (pipe(fds) != 0) {
            perror("pipe");
            return -1;
        }
        pipefds.push_back(fds[0]); 
        pipefds.push_back(fds[1]); 
    }

    std::vector<pid_t> pids(n, -1);

    for (size_t i = 0; i < n; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            for (int fd : pipefds) close(fd);
            return -1;
        }
        if (pid == 0) { 
            if (i > 0) {
                int read_fd = pipefds[(i - 1) * 2 + 0];
                if (dup2(read_fd, STDIN_FILENO) < 0) {
                    perror("dup2 stdin");
                    _exit(127);
                }
            }
            if (i + 1 < n) {
                int write_fd = pipefds[i * 2 + 1];
                if (dup2(write_fd, STDOUT_FILENO) < 0) {
                    perror("dup2 stdout");
                    _exit(127);
                }
            } else {
                if (redirect_out) {
                    int flags = O_WRONLY | O_CREAT;
                    if (out_type == OUTPUT_TYPE_FILE_NEW) flags |= O_TRUNC;
                    else flags |= O_APPEND;
                    int fd = open(out_file.c_str(), flags, 0644);
                    if (fd < 0) {
                        fprintf(stderr, "open(%s): %s\n", out_file.c_str(), strerror(errno));
                        _exit(127);
                    }
                    if (dup2(fd, STDOUT_FILENO) < 0) {
                        perror("dup2 outfile");
                        close(fd);
                        _exit(127);
                    }
                    close(fd);
                }
            }

            for (int fd : pipefds) close(fd);

            const command *c = cmds[i];
            if (c->exe == "cd") {
                if (c->args.empty()) {
                    _exit(0);
                } else {
                    if (chdir(c->args[0].c_str()) != 0) {
                        fprintf(stderr, "cd: %s: %s\n", c->args[0].c_str(), strerror(errno));
                        _exit(1);
                    }
                    _exit(0);
                }
            }
            if (c->exe == "exit") {
                int code = 0;
                if (!c->args.empty()) {
                    code = atoi(c->args[0].c_str());
                }
                _exit(code & 0xFF);
            }

            std::vector<char*> argv;
            argv.reserve(1 + c->args.size() + 1);
            argv.push_back(const_cast<char*>(c->exe.c_str()));
            for (const auto &a : c->args)
                argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);

            execvp(argv[0], argv.data());
            fprintf(stderr, "exec failed: %s: %s\n", argv[0], strerror(errno));
            _exit(127);
        } else {
            pids[i] = pid;
        }
    }

    for (int fd : pipefds) close(fd);


    if (background) {
    return 0;
	} else {
		int last_status = 0;
		pid_t last_pid = pids.back();
		int wstatus = 0;

		while (true) {
			pid_t w = waitpid(last_pid, &wstatus, 0);
			if (w == -1) {
				if (errno == EINTR) continue;
				perror("waitpid(last)");
				break;
			}
			if (WIFEXITED(wstatus)) last_status = WEXITSTATUS(wstatus);
			else if (WIFSIGNALED(wstatus)) last_status = 128 + WTERMSIG(wstatus);
			break;
		}

		while (true) {
			int st = 0;
			pid_t w = waitpid(-1, &st, 0);
			if (w == -1) {
				if (errno == EINTR) continue;
				if (errno == ECHILD) break;
				perror("waitpid");
				break;
			}
		}

		return last_status;
	}
}

static int
execute_command_line(const struct command_line *line)
{
    assert(line != NULL);

    struct segment {
        std::vector<const command *> cmds; 
        int sep_type; 
    };
    std::vector<segment> segments;
    segment cur;
    cur.sep_type = 0;

    for (const expr &e : line->exprs) {
        if (e.type == EXPR_TYPE_COMMAND) {
            const command *cmd = e.cmd.operator->();
            cur.cmds.push_back(cmd);
        } else if (e.type == EXPR_TYPE_PIPE) {
        } else if (e.type == EXPR_TYPE_AND) {
            segments.push_back(std::move(cur));
            cur = segment();
            cur.sep_type = 1;
        } else if (e.type == EXPR_TYPE_OR) {
            segments.push_back(std::move(cur));
            cur = segment();
            cur.sep_type = 2;
        } else {
            assert(false && "unexpected expr type");
        }
    }
    segments.push_back(std::move(cur));

    int last_status = 0;
    for (size_t si = 0; si < segments.size(); ++si) {
        const segment &seg = segments[si];
        if (seg.cmds.empty()) continue;

        bool is_last_segment = (si + 1 == segments.size());
        bool redirect_out = false;
        enum output_type out_type = OUTPUT_TYPE_STDOUT;
        std::string out_file;
        if (is_last_segment && line->out_type != OUTPUT_TYPE_STDOUT) {
            redirect_out = true;
            out_type = line->out_type;
            out_file = line->out_file;
        }

        if (seg.cmds.size() == 1) {
            const command *c = seg.cmds[0];
            if (c->exe == "cd") {
                if (c->args.empty()) {
                    last_status = 0;
                } else {
                    if (chdir(c->args[0].c_str()) != 0) {
                        fprintf(stderr, "cd: %s: %s\n", c->args[0].c_str(), strerror(errno));
                        last_status = 1;
                    } else {
                        last_status = 0;
                    }
                }
                goto after_exec_segment;
            }
        }

        last_status = run_pipeline(seg.cmds, redirect_out, out_type, out_file, line->is_background);

		if (!seg.cmds.empty()) {
            const command *last_cmd = seg.cmds.back();
            if (last_cmd->exe == "exit") {
                if (seg.cmds.size() == 1 && is_last_segment && !redirect_out && !line->is_background) {
                    exit(last_status & 0xFF);
                }
            }
        }

    	after_exec_segment:
        if (si + 1 < segments.size()) {
			int next_sep = segments[si + 1].sep_type;

			if (next_sep == 1 && last_status != 0) {    
				continue;  
			}

			if (next_sep == 2 && last_status == 0) {   
				continue;
			}
		}
    }

    return last_status & 0xFF;;
}

int
main(void)
{
	const size_t buf_size = 1024;
	char buf[buf_size];
	int rc;
	int last_status = 0; 
	struct parser *p = parser_new();
	while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
		parser_feed(p, buf, rc);
		struct command_line *line = NULL;
		while (true) {
			enum parser_error err = parser_pop_next(p, &line);
			if (err == PARSER_ERR_NONE && line == NULL)
				break;
			if (err != PARSER_ERR_NONE) {
				fprintf(stderr, "Parse error: %d\n", (int)err);
				continue;
			}
			last_status = execute_command_line(line) & 0xFF;
			delete line;
		}
	}
	parser_delete(p);
	return last_status & 0xFF;;
}