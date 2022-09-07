/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <LibMain/Main.h>
#include <LibTest/TestRunnerUtil.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

enum class TestResult {
    Passed,
    Failed,
    Skipped,
    MetadataError,
    HarnessError,
    TimeoutError,
    ProcessError,
    RunnerFailure,
    TodoError,
};

static StringView name_for_result(TestResult result)
{
    switch (result) {
    case TestResult::Passed:
        return "PASSED"sv;
    case TestResult::Failed:
        return "FAILED"sv;
    case TestResult::Skipped:
        return "SKIPPED"sv;
    case TestResult::MetadataError:
        return "METADATA_ERROR"sv;
    case TestResult::HarnessError:
        return "HARNESS_ERROR"sv;
    case TestResult::TimeoutError:
        return "TIMEOUT_ERROR"sv;
    case TestResult::ProcessError:
        return "PROCESS_ERROR"sv;
    case TestResult::RunnerFailure:
        return "RUNNER_EXCEPTION"sv;
    case TestResult::TodoError:
        return "TODO_ERROR"sv;
    }
    VERIFY_NOT_REACHED();
    return ""sv;
}

static TestResult result_from_string(StringView string_view)
{
    if (string_view == "passed"sv)
        return TestResult::Passed;
    if (string_view == "failed"sv)
        return TestResult::Failed;
    if (string_view == "skipped"sv)
        return TestResult::Skipped;
    if (string_view == "metadata_error"sv)
        return TestResult::MetadataError;
    if (string_view == "harness_error"sv)
        return TestResult::HarnessError;
    if (string_view == "timeout"sv)
        return TestResult::TimeoutError;
    if (string_view == "process_error"sv || string_view == "assert_fail"sv)
        return TestResult::ProcessError;
    if (string_view == "todo_error"sv)
        return TestResult::TodoError;

    return TestResult::RunnerFailure;
}

static StringView emoji_for_result(TestResult result)
{
    switch (result) {
    case TestResult::Passed:
        return "✅"sv;
    case TestResult::Failed:
        return "❌"sv;
    case TestResult::Skipped:
        return "⚠"sv;
    case TestResult::MetadataError:
        return "📄"sv;
    case TestResult::HarnessError:
        return "⚙"sv;
    case TestResult::TimeoutError:
        return "💀"sv;
    case TestResult::ProcessError:
        return "💥"sv;
    case TestResult::RunnerFailure:
        return "🐍"sv;
    case TestResult::TodoError:
        return "📝"sv;
    }
    VERIFY_NOT_REACHED();
    return ""sv;
}

static constexpr StringView total_test_emoji = "🧪"sv;

class Test262RunnerHandler {
public:
    static OwnPtr<Test262RunnerHandler> create(char const* const command[])
    {
        int write_pipe_fds[2];
        int read_pipe_fds[2];
        if (pipe2(write_pipe_fds, O_CLOEXEC) < 0) {
            perror("pipe2");
            return nullptr;
        }

        if (pipe2(read_pipe_fds, O_CLOEXEC) < 0) {
            perror("pipe2");
            return nullptr;
        }

        posix_spawn_file_actions_t file_actions;
        posix_spawn_file_actions_init(&file_actions);
        posix_spawn_file_actions_adddup2(&file_actions, write_pipe_fds[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&file_actions, read_pipe_fds[1], STDOUT_FILENO);

        pid_t pid {};

        if (posix_spawnp(&pid, command[0], &file_actions, nullptr, const_cast<char**>(command), environ) < 0) {
            perror("posix_spawnp");
            return nullptr;
        }

        posix_spawn_file_actions_destroy(&file_actions);

        close(write_pipe_fds[0]);
        close(read_pipe_fds[1]);

        auto infile = Core::File::construct();
        infile->open(read_pipe_fds[0], Core::OpenMode::ReadOnly, Core::File::ShouldCloseFileDescriptor::Yes);

        auto outfile = Core::File::construct();
        outfile->open(write_pipe_fds[1], Core::OpenMode::WriteOnly, Core::File::ShouldCloseFileDescriptor::Yes);

        return make<Test262RunnerHandler>(pid, move(infile), move(outfile));
    }

    Test262RunnerHandler(pid_t pid, NonnullRefPtr<Core::File> in_file, NonnullRefPtr<Core::File> out_file)
        : m_pid(pid)
        , m_input(move(in_file))
        , m_output(move(out_file))
    {
    }

    bool write_lines(Span<String> lines)
    {
        // It's possible the process dies before we can write all the tests
        // to the stdin. So make sure that
        struct sigaction act { };
        struct sigaction oldAct;
        act.sa_flags = 0;
        act.sa_handler = SIG_IGN;
        if (sigaction(SIGPIPE, &act, &oldAct) < 0) {
            perror("sigaction");
            return false;
        }

        for (String const& line : lines) {
            if (!m_output->write(String::formatted("{}\n", line)))
                break;
        }

        // It's not really a problem if this signal failed
        if (sigaction(SIGPIPE, &oldAct, nullptr) < 0)
            perror("sigaction");

        return true;
    }

    String read_all()
    {
        return String(m_input->read_all().bytes(), Chomp);
    }

    enum class ProcessResult {
        Running,
        DoneWithZeroExitCode,
        Failed,
        FailedFromTimeout,
        Unknown,
    };

    ProcessResult status()
    {
        if (m_pid == -1)
            return ProcessResult::Unknown;

        int wstatus { 0 };
        int rc = waitpid(m_pid, &wstatus, WNOHANG);
        if (rc == 0) {
            // Attempt to kill it, since it has not finished yet somehow
            return ProcessResult::Running;
        }
        m_pid = -1;

        if (WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGALRM)
            return ProcessResult::FailedFromTimeout;

        if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0)
            return ProcessResult::DoneWithZeroExitCode;

        return ProcessResult::Failed;
    }

public:
    pid_t m_pid;
    NonnullRefPtr<Core::File> m_input;
    NonnullRefPtr<Core::File> m_output;
};

static HashMap<size_t, TestResult> run_test_files(Span<String> files, size_t offset, char const* const command[])
{
    HashMap<size_t, TestResult> results {};
    results.ensure_capacity(files.size());
    size_t test_index = 0;

    auto fail_all_after = [&] {
        for (; test_index < files.size(); ++test_index)
            results.set(offset + test_index, TestResult::RunnerFailure);
    };

    while (test_index < files.size()) {
        auto runner_process = Test262RunnerHandler::create(command);
        if (!runner_process) {
            fail_all_after();
            return results;
        }

        if (!runner_process->write_lines(files.slice(test_index))) {
            fail_all_after();
            return results;
        }

        String output = runner_process->read_all();
        auto status = runner_process->status();
        VERIFY(status != Test262RunnerHandler::ProcessResult::Running);
        bool failed = status != Test262RunnerHandler::ProcessResult::DoneWithZeroExitCode;

        for (StringView line : output.split_view('\n')) {
            if (!line.starts_with("RESULT "sv))
                break;

            auto test_for_line = test_index;
            ++test_index;
            if (test_for_line >= files.size())
                break;

            line = line.substring_view(7).trim("\n\0 "sv);
            JsonParser parser { line };
            TestResult result = TestResult::RunnerFailure;
            auto result_object_or_error = parser.parse();
            if (!result_object_or_error.is_error() && result_object_or_error.value().is_object()) {
                auto& result_object = result_object_or_error.value().as_object();
                if (auto result_string = result_object.get_ptr("result"sv); result_string && result_string->is_string()) {
                    auto const& view = result_string->as_string();
                    // Timeout and assert fail already are the result of the stopping test
                    if (view == "timeout"sv || view == "assert_fail"sv) {
                        failed = false;
                    }

                    result = result_from_string(view);
                }
            }

            results.set(test_for_line + offset, result);
        }

        if (failed) {
            TestResult result = TestResult::ProcessError;
            if (status == Test262RunnerHandler::ProcessResult::FailedFromTimeout) {
                result = TestResult::TimeoutError;
            }
            // assume the last test failed, if by SIGALRM signal it's a timeout
            results.set(test_index + offset, result);
            ++test_index;
        }
    }

    return results;
}

void write_per_file(HashMap<size_t, TestResult> const& result_map, Vector<String> const& paths, StringView per_file_name, double time_taken_in_ms);

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    size_t batch_size = 50;
    StringView per_file_location;
    StringView pass_through_parameters;
    StringView runner_command = "test262-runner"sv;
    char const* test_directory = nullptr;
    bool dont_print_progress = false;
    bool dont_disable_core_dump = false;

    Core::ArgsParser args_parser;
    args_parser.add_positional_argument(test_directory, "Directory to search for tests", "tests");
    args_parser.add_option(per_file_location, "Output a per-file containing all results", "per-file", 'o', "filename");
    args_parser.add_option(batch_size, "Size of batches send to runner at once", "batch-size", 'b', "batch size");
    args_parser.add_option(runner_command, "Command to run", "runner-command", 'r', "command");
    args_parser.add_option(pass_through_parameters, "Parameters to pass through to the runner, will split on spaces", "pass-through", 'p', "parameters");
    args_parser.add_option(dont_print_progress, "Hide progress information", "quiet", 'q');
    args_parser.add_option(dont_disable_core_dump, "Enabled core dumps for runner (i.e. don't pass --disable-core-dump)", "enable-core-dumps", 0);
    args_parser.parse(arguments);

    // Normalize the path to ensure filenames are consistent
    Vector<String> paths;

    if (!Core::File::is_directory(test_directory)) {
        paths.append(test_directory);
    } else {
        Test::iterate_directory_recursively(LexicalPath::canonicalized_path(test_directory), [&](String const& file_path) {
            if (file_path.contains("_FIXTURE"sv))
                return;
            // FIXME: Add ignored file set
            paths.append(file_path);
        });
        quick_sort(paths);
    }

    outln("Found {} tests", paths.size());

    auto parameters = pass_through_parameters.split_view(' ');
    Vector<String> args;
    args.ensure_capacity(parameters.size() + 2);
    args.append(runner_command);
    if (!dont_disable_core_dump)
        args.append("--disable-core-dump"sv);

    for (auto parameter : parameters)
        args.append(parameter);

    Vector<char const*> raw_args;
    raw_args.ensure_capacity(args.size() + 1);
    for (auto& arg : args)
        raw_args.append(arg.characters());

    raw_args.append(nullptr);

    dbgln("test262 runner command: {}", args);

    HashMap<size_t, TestResult> results;
    Array<size_t, 9> result_counts {};
    static_assert(result_counts.size() == static_cast<size_t>(TestResult::TodoError) + 1u);
    size_t index = 0;

    double start_time = Test::get_time_in_ms();

    auto print_progress = [&] {
        if (!dont_print_progress) {
            warn("\033]9;{};{};\033\\", index, paths.size());
            double percentage_done = (100. * index) / paths.size();
            warn("{:04.2f}% {:3.1f}s ", percentage_done, (Test::get_time_in_ms() - start_time) / 1000.);
            for (size_t i = 0; i < result_counts.size(); ++i) {
                auto result_type = static_cast<TestResult>(i);
                warn("{} {} ", emoji_for_result(result_type), result_counts[i]);
            }
            warn("\r");
        }
    };

    while (index < paths.size()) {
        print_progress();
        auto this_batch_size = min(batch_size, paths.size() - index);
        auto batch_results = run_test_files(paths.span().slice(index, this_batch_size), index, raw_args.data());

        results.ensure_capacity(results.size() + batch_results.size());
        for (auto& [key, value] : batch_results) {
            results.set(key, value);
            ++result_counts[static_cast<size_t>(value)];
        }

        index += this_batch_size;
    }

    double time_taken_in_ms = Test::get_time_in_ms() - start_time;

    print_progress();
    if (!dont_print_progress)
        warn("\n\033]9;-1;\033\\");

    outln("Took {} seconds", time_taken_in_ms / 1000.);
    outln("{}: {}", total_test_emoji, paths.size());
    for (size_t i = 0; i < result_counts.size(); ++i) {
        auto result_type = static_cast<TestResult>(i);
        outln("{}: {} ({:3.2f}%)", emoji_for_result(result_type), result_counts[i], 100. * static_cast<double>(result_counts[i]) / paths.size());
    }

    if (!per_file_location.is_empty())
        write_per_file(results, paths, per_file_location, time_taken_in_ms);

    return 0;
}

void write_per_file(HashMap<size_t, TestResult> const& result_map, Vector<String> const& paths, StringView per_file_name, double time_taken_in_ms)
{

    auto file_or_error = Core::File::open(per_file_name, Core::OpenMode::WriteOnly);
    if (file_or_error.is_error()) {
        warnln("Failed to open per file for writing at {}: {}", per_file_name, file_or_error.error().string_literal());
        return;
    }

    auto& file = file_or_error.value();

    JsonObject result_object;
    for (auto& [test, value] : result_map)
        result_object.set(paths[test], name_for_result(value));

    JsonObject complete_results {};
    complete_results.set("duration", time_taken_in_ms / 1000.);
    complete_results.set("results", result_object);

    if (!file->write(complete_results.to_string()))
        warnln("Failed to write per-file");
    file->close();
}