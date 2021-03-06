/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "skip/System.h"
#include "skip/System-extc.h"

#include "skip/AllocProfiler.h"
#include "skip/Exception.h"
#include "skip/LockManager.h"
#include "skip/Process.h"
#include "skip/String.h"
#include "skip/external.h"
#include "skip/memoize.h"
#include "skip/parallel.h"
#include "skip/util.h"
#include "skip/map.h"

#include <chrono>
#include <cmath>
#include <iostream>

using namespace skip;

namespace {

#if __APPLE__
// On Mac the main thread is 8MB and worker stacks are 512k by default
const size_t kSkipWorkerThreadStackSize = 8ULL * 1024 * 1024;
#endif

std::string s_cppArgument0; // argv[0]
std::vector<std::string> s_cppArguments; // argv[1..argc-1]

// A unique id for this skip process instance, useful for isolating
// sample sets for compiler logs. TODO: make this more than just a timestamp.
ssize_t s_skid;

} // namespace

ssize_t skip::getSkid() {
  return s_skid;
}

void skip::initializeNormalThread() {
  Obstack::threadInit();
  (void)lockManager();
}

void skip::initializeThreadWithPermanentProcess() {
  initializeNormalThread();
  Process::installPermanently();
}

void skip::initializeSkip(int argc, char** argv) {
  // Run some initializers in the proper order
  // Catch-22 - We need to compute the args before calling
  // SKIP_initializeSkip because SKIP_initializeSkip can legally ask for
  // the args.  But we need to call SKIP_initializeSkip before we can ask
  // skip to allocate a vector.  So store the args in C++ structures to be
  // converted later on demand.
  // Note, too, that we process command line arguments before Arena
  // initialization because we want to ensure AllocProfiler::init()
  // is called before Arena and Obstack initialization.
  s_cppArgument0 = std::string(argv[0]);
  for (int i = 1; i < argc; ++i) {
    s_cppArguments.emplace_back(argv[i]);
  }
  // HACK: See T28176670 for some thoughts on how we might avoid this hack:
  auto isSkipCompiler = ends_with(s_cppArgument0, "skip_to_llvm");
  AllocProfiler::init(isSkipCompiler);

  (void)Arena::KindMapper::singleton();
  Arena::init();
  getInternTable();
}

String SKIP_Int_toString(SkipInt i) {
  if (i > -1000000 && i < 10000000) {
    // Short string
    bool negative = i < 0;
    auto value = static_cast<uint32_t>(negative ? -i : i);
    int64_t res = 0;
    int64_t sizemask = -(String::MAX_SHORT_LENGTH + 1);
    do {
      res = (res << 8) + (value % 10 + '0');
      value /= 10;
      ++sizemask;
    } while (value > 0);

    if (negative) {
      res = (res << 8) + '-';
      ++sizemask;
    }
    res |= static_cast<uint64_t>(sizemask) << String::SHORT_LENGTH_SHIFT;
    return reinterpret_cast<String&>(res);

  } else if (i == -9223372036854775807LL - 1) {
    return String("-9223372036854775808");

  } else {
    // Long string
    std::array<char, 20> buffer;
    char* p = buffer.end();

    bool negative = i < 0;
    auto value = static_cast<uint64_t>(negative ? -i : i);

    do {
      *--p = value % 10 + '0';
      value /= 10;
    } while (value > 0);

    if (negative)
      *--p = '-';
    return String(p, buffer.end());
  }
}

String SKIP_Float_toString(double d) {
  if (std::isnan(d)) {
    // Technically there's a difference between +nan and -nan - but we're going
    // to ignore that for now since the front end is giving us -nan.
    return String("nan");
  } else if (std::isinf(d)) {
    return String(std::signbit(d) ? "-inf" : "inf");
  }

  // FIXME: This is just plain wrong.  Note that Python has its own conversion
  // for a good reason.
  std::array<char, 100> buf;
  size_t len = snprintf(buf.data(), buf.size(), "%.17g", d);
  auto i = std::find_if(buf.begin(), buf.begin() + len, [](char c) {
    return !(((c >= '0') && (c <= '9')) || c == '-');
  });
  if (i == buf.begin() + len) {
    buf[len++] = '.';
    buf[len++] = '0';
  }
  return String(buf.begin(), buf.begin() + len);
}

void SKIP_print_stack_trace() {
  printStackTrace();
}

void SKIP_print_raw(String str) {
#if SKIP_PARANOID
  if (auto p = const_cast<LongString*>(str.asLongString())) {
    assert(
        p->metadata().m_hash == String::computeStringHash(p, str.byteSize()));
  }
#endif

  skip::String::DataBuffer buf;
  std::fwrite(str.data(buf), 1, str.byteSize(), stdout);
}

void SKIP_flush_stdout() {
  std::fflush(stdout);
}

void SKIP_print_last_exception_stack_trace_and_exit(void* /*exc*/) {
  // TODO: We do not yet know what "exc" is."
  fatal("Uncaught exception");
}

void SKIP_debug_break(void) {}

const char* SkipException::what() const noexcept {
  if (m_whatBuffer.empty()) {
    try {
      m_whatBuffer = String(SKIP_getExceptionMessage(
                                reinterpret_cast<RObj*>(m_skipException)))
                         .toCppString();
    } catch (...) {
      m_whatBuffer = "exception";
    }
  }
  return m_whatBuffer.c_str();
}

void SKIP_throw(RObj* exc) {
  throw SkipException(exc);
}

namespace profile {

// Our profile results return milliseconds but we do our calculation in
// nanoseconds so we can have a more precise measurement.
// On linux gcc or clang, high_resolution_clock is an alias of system_clock,
// which is wall-time (vs global or thread cpu-time).
using clock_t = std::chrono::nanoseconds;
using clock_spec = std::chrono::high_resolution_clock;

thread_local clock_t duration;
thread_local std::chrono::time_point<clock_spec> lastStart;
thread_local std::vector<double> records;
} // namespace profile

void SKIP_profile_start() {
  profile::duration = profile::clock_t(0);
  profile::lastStart = profile::clock_spec::now();
}

double SKIP_profile_stop() {
  profile::duration += profile::clock_spec::now() - profile::lastStart;
  constexpr double divisor = std::chrono::milliseconds(1) / profile::clock_t(1);
  double elapsed = (double)profile::duration.count() / divisor;
  profile::records.push_back(elapsed);
  return elapsed;
}

void SKIP_profile_report() {
  for (auto t : profile::records) {
    std::cout << t << std::endl;
  }
  profile::records.clear();
}

SkipInt SKIP_nowNanos() {
  return profile::clock_spec::now().time_since_epoch() /
      std::chrono::nanoseconds(1);
}

const RObj* SKIP_arguments() {
  static std::once_flag s_flag;
  static IObjPtr s_arguments;
  call_once(s_flag, []() {
    Obstack::PosScope gc;
    auto& p = *createStringVector(s_cppArguments.size());
    for (int i = 0; i < s_cppArguments.size(); ++i) {
      p[i] = String(s_cppArguments[i]);
    }
    s_arguments.reset(intern(&p));
  });
  return reinterpret_cast<const RObj*>(s_arguments.get());
}

String SKIP_getcwd() {
  std::array<char, PATH_MAX> buf;
  const char* p = getcwd(buf.data(), buf.size());
  if (!p) {
    throwRuntimeError("getcwd() failed, errno=%d", errno);
  }
  return String(p);
}

void SKIP_internalExit(SkipInt result) {
  // This is an exception which skip user code can't catch
  throw SkipExitException{(int)result};
}

const char* SkipExitException::what() const noexcept {
  return "exit exception";
}

void SKIP_print_error(String s) {
  String::DataBuffer buf;
  auto piece = String(s).slice(buf);
  fwrite(piece.begin(), 1, piece.size(), stderr);
}

void SKIP_unreachable() {
  fputs("Internal error, executed unreachable code.\n", stderr);
  abort();
}

void SKIP_unreachableWithExplanation(const char* why) {
  fprintf(stderr, "Internal error, executed unreachable code: %s\n", why);
  abort();
}

void SKIP_unreachableMethodCall(String why, RObj* robj) {
  String::CStrBuffer buf;
  fprintf(
      stderr,
      "Internal error, executed unreachable code: %s. "
      "The instance was actually of type %s.\n",
      String(why).c_str(buf),
      robj->type().name());
  abort();
}
