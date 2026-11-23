/**
 * Runs a GStreamer pipeline and reports its bus messages until the end of the
 * stream, an error, a timeout, or Ctrl+C. Without a pipeline description, it
 * plays a bouncing ball test pattern.
 *
 * Usage:
 *   play [options] [pipeline description]
 *
 * Examples:
 *   play --timeout 5 audiotestsrc num-buffers=100 ! audioconvert !
 * autoaudiosink play --verbose
 */

#include "handle.h"
#include <gst/gst.h>
#include <glib.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <concepts>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <optional>
#include <ostream>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <io.h>
// The Microsoft C runtime has no POSIX names for the standard descriptors.
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#else
#include <unistd.h>
#endif

GST_DEBUG_CATEGORY_STATIC(play_debug);

// GST_CAT_DEFAULT routes GST_LOG() and friends to the category above.
// PLAY_CHECK() turns a failed GStreamer call into an exception that names the
// call; stringizing the call is why it is a macro.
#define GST_CAT_DEFAULT play_debug
#define PLAY_CHECK(call)                         \
  do {                                           \
    if (!(call)) {                               \
      throw std::runtime_error{#call " failed"}; \
    }                                            \
  } while (false)

namespace gstu {

// handle.h only lists the common mini objects. Tag lists are mini objects too,
// so opt them in.
template<>
inline constexpr bool enable_gst_mini_object<GstTagList> = true;

}  // namespace gstu

// Lets std::format() print GStreamer states and state change results by name.
template<typename T>
  requires(std::same_as<T, GstState> || std::same_as<T, GstStateChangeReturn>)
struct std::formatter<T> : std::formatter<std::string_view> {
  auto format(T value, std::format_context& context) const
  {
    if constexpr (std::same_as<T, GstState>) {
      return std::formatter<std::string_view>::format(gst_element_state_get_name(value), context);
    } else {
      return std::formatter<std::string_view>::format(gst_element_state_change_return_get_name(value), context);
    }
  }
};

namespace play {
namespace {

using gstu::adopt;
using gstu::adopt_floating;
using gstu::share;

using Clock = std::chrono::steady_clock;

// Set by the SIGINT handler and polled by the bus loop; only lock-free atomics
// are signal safe.
std::atomic<bool> interrupt_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

// How a run ended. Each outcome has its own exit status, so that scripts can
// tell them apart.
enum class Outcome { end_of_stream, error, timed_out, interrupted };

constexpr std::string_view describe(Outcome outcome) noexcept
{
  switch (outcome) {
    case Outcome::end_of_stream:
      return "end of stream";
    case Outcome::error:
      return "error";
    case Outcome::timed_out:
      return "timed out";
    case Outcome::interrupted:
      return "interrupted";
  }
  std::unreachable();
}

constexpr int exit_status(Outcome outcome) noexcept
{
  switch (outcome) {
    case Outcome::end_of_stream:
      return EXIT_SUCCESS;
    case Outcome::error:
      return EXIT_FAILURE;
    case Outcome::timed_out:
      return 124;  // what timeout(1) returns
    case Outcome::interrupted:
      return 128 + SIGINT;  // what shells report for a fatal signal
  }
  std::unreachable();
}

constexpr int usage_exit_status = 2;  // what getopt based tools return for a bad command line

// A command line that does not make sense; main() answers it with the usage.
class Usage_error : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

// An error reported by GStreamer, with the debug details it adds for
// developers.
class Pipeline_error : public std::runtime_error {
public:
  Pipeline_error(std::string_view context, const GError* error, const char* debug_details) :
      std::runtime_error{std::format("{}: {}", context, error ? error->message : "unknown error")},
      debug_details_{debug_details ? debug_details : ""}
  {
  }

  [[nodiscard]] const std::string& debug_details() const noexcept { return debug_details_; }

private:
  std::string debug_details_;
};

struct Options {
  std::vector<const char*> description;  // the pipeline description, as the shell split it
  std::chrono::seconds timeout{0};       // zero waits for the end of the stream, however long it takes
  bool verbose = false;
  bool show_help = false;
};

struct Option_spec {
  std::string_view flags;
  std::string_view help;
};

constexpr std::array option_specs{
  Option_spec{"-t, --timeout <seconds>",
              "stop after the given number of seconds; the default, 0, waits "
              "for the end of the stream"},
  Option_spec{"-v, --verbose",
              "also report the state changes of every element, the stream "
              "tags, and debug details"},
  Option_spec{"-h, --help", "print this help and exit"},
};

constexpr std::string_view usage_header =
  "Usage: play [options] [pipeline description]\n"
  "\n"
  "Runs a GStreamer pipeline, a test pattern by default, and reports its bus "
  "messages.\n"
  "\n"
  "Options:\n";

void print_usage(std::ostream& out)
{
  out << usage_header;
  const auto width =
    std::ranges::max(option_specs | std::views::transform([](const Option_spec& spec) { return spec.flags.size(); }));
  for (const auto& spec : option_specs) out << std::format("  {:<{}}  {}\n", spec.flags, width, spec.help);
}

[[nodiscard]]
std::chrono::seconds parse_seconds(std::string_view text)
{
  unsigned value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size())
    throw Usage_error{std::format("'{}' is not a number of seconds", text)};
  return std::chrono::seconds{value};
}

// gst_init() has already removed the --gst-* options, so everything left is
// ours.
[[nodiscard]]
Options parse_arguments(std::span<char* const> arguments)
{
  Options options;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const std::string_view argument = arguments[index];
    if (argument == "-h" || argument == "--help") {
      options.show_help = true;
    } else if (argument == "-v" || argument == "--verbose") {
      options.verbose = true;
    } else if (argument == "-t" || argument == "--timeout") {
      if (++index == arguments.size()) throw Usage_error{std::format("{} needs a number of seconds", argument)};
      options.timeout = parse_seconds(arguments[index]);
    } else if (argument.starts_with('-')) {
      throw Usage_error{std::format("unknown option '{}'", argument)};
    } else {
      options.description.push_back(arguments[index]);
    }
  }
  return options;
}

// ANSI escape sequences that color the labels; left empty when the output is
// not a terminal.
struct Palette {
  std::string_view error;
  std::string_view warning;
  std::string_view reset;
};

constexpr Palette ansi_palette{.error = "\033[1;31m", .warning = "\033[1;33m", .reset = "\033[0m"};
constexpr Palette plain_palette{};

[[nodiscard]]
bool is_terminal(int descriptor) noexcept
{
#ifdef _WIN32
  return _isatty(descriptor) != 0;
#else
  return isatty(descriptor) != 0;
#endif
}

// gst_parse_launchv() can build a usable pipeline and still report a
// recoverable error, such as an unknown property, so the result carries both.
struct Parsed_pipeline {
  gstu::Element_ptr pipeline;
  gstu::Error_ptr error;
};

[[nodiscard]]
Parsed_pipeline parse_pipeline(std::vector<const char*> words)
{
  words.push_back(nullptr);  // gst_parse_launchv() expects a null terminated array
  Parsed_pipeline parsed;
  parsed.pipeline = adopt_floating(gst_parse_launchv(words.data(), gstu::out_ptr(parsed.error)));
  if (!parsed.pipeline) throw Pipeline_error{"invalid pipeline description", parsed.error.get(), nullptr};
  return parsed;
}

// The default pipeline: an orange ball bouncing over a dark background for 20
// seconds.
[[nodiscard]]
gstu::Element_ptr make_test_pipeline()
{
  auto pipeline = adopt_floating(gst_pipeline_new("test-pattern"));
  const auto source = adopt_floating(gst_element_factory_make("videotestsrc", nullptr));
  const auto convert = adopt_floating(gst_element_factory_make("videoconvert", nullptr));
  const auto sink = adopt_floating(gst_element_factory_make("autovideosink", nullptr));
  if (!source || !convert || !sink)
    throw std::runtime_error{
      "the test pattern needs the videotestsrc, videoconvert, and "
      "autovideosink elements"};

  gst_util_set_object_arg(G_OBJECT(source.get()), "pattern", "ball");
  g_object_set(source.get(), "num-buffers", 600, "foreground-color", 0xffff8000U, "background-color", 0xff202020U,
               "is-live", TRUE, nullptr);

  gst_bin_add_many(GST_BIN(pipeline.get()), source.get(), convert.get(), sink.get(), nullptr);
  PLAY_CHECK(gst_element_link_many(source.get(), convert.get(), sink.get(), nullptr));
  return pipeline;
}

// Keeps the pipeline playing for the lifetime of the scope. GStreamer requires
// a pipeline to be back in the NULL state before its last reference goes away,
// whatever the exit path.
class Playing_scope {
public:
  explicit Playing_scope(GstElement& pipeline) noexcept :
      pipeline_{share(&pipeline)}, result_{gst_element_set_state(pipeline_.get(), GST_STATE_PLAYING)}
  {
  }
  ~Playing_scope() { gst_element_set_state(pipeline_.get(), GST_STATE_NULL); }

  Playing_scope(const Playing_scope&) = delete;
  Playing_scope& operator=(const Playing_scope&) = delete;

  // What gst_element_set_state() answered:
  //   - ASYNC while the pipeline prerolls,
  //   - NO_PREROLL for live sources, which cannot preroll,
  //   - FAILURE, with the reason posted on the bus.
  [[nodiscard]] GstStateChangeReturn result() const noexcept { return result_; }

private:
  gstu::Element_ptr pipeline_;
  GstStateChangeReturn result_;
};

// What the bus reported over the run, printed once the loop is over.
struct Summary {
  std::size_t messages = 0;
  std::size_t warnings = 0;
  bool tagged : 1 = false;      // the stream carried tags, such as a title or a codec name
  bool clock_lost : 1 = false;  // the clock provider went away, e.g. an unplugged audio device
};

// Anything that looks at a bus message and may decide that the run is over.
template<typename T>
concept Message_handler = requires(T& handler, GstMessage& message) {
  { handler(message) } -> std::same_as<std::optional<Outcome>>;
};

// Pops bus messages until the handler returns an outcome, the deadline passes,
// or Ctrl+C is pressed.
template<Message_handler Handler>
Outcome pump_bus(GstBus& bus, Clock::time_point deadline,
                 Handler&& handler) noexcept(std::is_nothrow_invocable_v<Handler&, GstMessage&>)
{
  // Short enough for Ctrl+C and the deadline to feel immediate, in nanoseconds
  // like any GstClockTime.
  constexpr GstClockTime poll_interval = 100000000;
  std::optional<Outcome> outcome;
  do {
    if (interrupt_requested.load()) {
      outcome = Outcome::interrupted;
    } else if (Clock::now() >= deadline) {
      outcome = Outcome::timed_out;
    } else if (const auto message = adopt(gst_bus_timed_pop(&bus, poll_interval))) {
      outcome = handler(*message);
    }
  } while (!outcome);
  return *outcome;
}

// Reports the bus messages worth a line and decides when the run is over.
class Bus_reporter {
public:
  Bus_reporter(GstElement& pipeline, std::ostream& out, Palette palette, bool verbose) :
      pipeline_{share(&pipeline)}, out_{&out}, palette_{palette}, verbose_{verbose}
  {
  }

  std::optional<Outcome> operator()(GstMessage& message);

  [[nodiscard]] const Summary& summary() const noexcept { return summary_; }

private:
  [[nodiscard]] bool is_worth_a_line(const GstObject* source, GstState old_state, GstState new_state,
                                     GstState pending_state) const noexcept;
  void report_problem(GstMessage& message);
  void report_state_change(GstMessage& message);
  void report_tags(GstMessage& message);

  gstu::Element_ptr pipeline_;
  std::ostream* out_;
  Palette palette_;
  bool verbose_;
  Summary summary_;
};

std::optional<Outcome> Bus_reporter::operator()(GstMessage& message)
{
  ++summary_.messages;
  switch (GST_MESSAGE_TYPE(&message)) {
    case GST_MESSAGE_EOS:
      *out_ << "end of stream\n";
      return Outcome::end_of_stream;
    case GST_MESSAGE_ERROR:
      report_problem(message);
      return Outcome::error;
    case GST_MESSAGE_WARNING:
      ++summary_.warnings;
      report_problem(message);
      break;
    case GST_MESSAGE_STATE_CHANGED:
      report_state_change(message);
      break;
    case GST_MESSAGE_TAG:
      summary_.tagged = true;
      if (verbose_) report_tags(message);
      break;
    case GST_MESSAGE_CLOCK_LOST: {
      // Pausing and resuming makes the pipeline select a new clock.
      GstElement* const pipeline = pipeline_.get();
      summary_.clock_lost = true;
      gst_element_set_state(pipeline, GST_STATE_PAUSED);
      gst_element_set_state(pipeline, GST_STATE_PLAYING);
      break;
    }
    default:
      break;
  }
  return std::nullopt;
}

// Without --verbose, only the pipeline's own transitions are worth a line, once
// they have settled.
bool Bus_reporter::is_worth_a_line(const GstObject* source, GstState old_state, GstState new_state,
                                   GstState pending_state) const noexcept
{
  return (verbose_ || (source == GST_OBJECT_CAST(pipeline_.get()) && new_state != old_state &&
                       pending_state == GST_STATE_VOID_PENDING));
}

void Bus_reporter::report_problem(GstMessage& message)
{
  gstu::Error_ptr problem;
  gstu::Chars_ptr debug_details;
  const bool is_error = GST_MESSAGE_TYPE(&message) == GST_MESSAGE_ERROR;
  if (is_error) {
    gst_message_parse_error(&message, gstu::out_ptr(problem), gstu::out_ptr(debug_details));
  } else {
    gst_message_parse_warning(&message, gstu::out_ptr(problem), gstu::out_ptr(debug_details));
  }

  const std::string_view color = is_error ? palette_.error : palette_.warning;
  const std::string_view label = is_error ? "error" : "warning";
  *out_ << std::format("{}{}{} from {}: {}\n", color, label, palette_.reset, GST_MESSAGE_SRC_NAME(&message),
                       problem->message);
  if (verbose_ && debug_details) *out_ << std::format("  {}\n", debug_details.get());
}

void Bus_reporter::report_state_change(GstMessage& message)
{
  GstState old_state = GST_STATE_VOID_PENDING;
  GstState new_state = GST_STATE_VOID_PENDING;
  GstState pending_state = GST_STATE_VOID_PENDING;
  gst_message_parse_state_changed(&message, &old_state, &new_state, &pending_state);
  if (!is_worth_a_line(GST_MESSAGE_SRC(&message), old_state, new_state, pending_state)) return;

  *out_ << std::format("{}: {} -> {}{}\n", GST_MESSAGE_SRC_NAME(&message), old_state, new_state,
                       pending_state == GST_STATE_VOID_PENDING ? "" : std::format(" (heading for {})", pending_state));
}

void Bus_reporter::report_tags(GstMessage& message)
{
  gstu::Handle<GstTagList> tags;
  // "transfer full": false makes intrusive_ptr::reset() adopt the reference
  // instead of adding one.
  gst_message_parse_tag(&message, gstu::out_ptr(tags, false));
  gst_tag_list_foreach(
    tags.get(),
    [](const GstTagList* list, const gchar* tag, gpointer user_data) {
      const gstu::Chars_ptr value{gst_value_serialize(gst_tag_list_get_value_index(list, tag, 0))};
      *static_cast<std::ostream*>(user_data) << std::format("  {:<20}{}\n", tag, value ? value.get() : "(unprintable)");
    },
    out_);
}

[[nodiscard]]
Outcome run(const Options& options, Palette palette)
{
  auto parsed =
    options.description.empty() ? Parsed_pipeline{make_test_pipeline(), nullptr} : parse_pipeline(options.description);
  if (parsed.error)
    std::cout << std::format("{}warning{}: {}\n", palette.warning, palette.reset, parsed.error->message);

  const auto bus = adopt(gst_element_get_bus(parsed.pipeline.get()));
  Bus_reporter reporter{*parsed.pipeline, std::cout, palette, options.verbose};
  const Playing_scope playing{*parsed.pipeline};
  if (options.verbose) std::cout << std::format("starting: {}\n", playing.result());

  // A zero timeout waits for the end of the stream, however long that takes.
  const auto deadline = [&]() -> Clock::time_point {
    if (options.timeout == std::chrono::seconds::zero()) return Clock::time_point::max();
    return Clock::now() + options.timeout;
  }();

  const auto started = Clock::now();
  const auto outcome = pump_bus(*bus, deadline, [&](GstMessage& message) {
    GST_LOG("%s from %s", GST_MESSAGE_TYPE_NAME(&message), GST_MESSAGE_SRC_NAME(&message));
    return reporter(message);
  });
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);

  const auto& summary = reporter.summary();
  std::cout << std::format("{} after {:%T}: {} messages, {} warnings{}{}\n", describe(outcome), elapsed,
                           summary.messages, summary.warnings, summary.tagged ? ", tagged" : "",
                           summary.clock_lost ? ", clock lost" : "");
  return outcome;
}

}  // namespace
}  // namespace play

extern "C" {
// Ctrl+C stops the bus loop instead of killing the process, so the pipeline
// still shuts down cleanly.
static void on_interrupt(int /*signal*/) { play::interrupt_requested.store(true); }
}

int main(int argc, char* argv[])
{
  // gst_init() removes the --gst-* options it handles, so it runs before our
  // own parsing.
  gst_init(&argc, &argv);
  GST_DEBUG_CATEGORY_INIT(play_debug, "play", 0, "gstu play example");
  std::signal(SIGINT, on_interrupt);

  try {
    const auto options = play::parse_arguments(std::span{argv, static_cast<std::size_t>(argc)}.subspan(1));
    if (options.show_help) {
      play::print_usage(std::cout);
      return EXIT_SUCCESS;
    }
    const auto& palette = play::is_terminal(STDOUT_FILENO) ? play::ansi_palette : play::plain_palette;
    return play::exit_status(play::run(options, palette));
  } catch (const play::Usage_error& error) {
    std::cerr << "play: " << error.what() << "\n\n";
    play::print_usage(std::cerr);
    return play::usage_exit_status;
  } catch (const play::Pipeline_error& error) {
    std::cerr << "play: " << error.what() << '\n';
    if (!error.debug_details().empty()) std::cerr << error.debug_details() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "play: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
