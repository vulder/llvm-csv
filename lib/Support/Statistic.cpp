//===-- Statistic.cpp - Easy way to expose stats information --------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the 'Statistic' class, which is designed to be an easy
// way to expose various success metrics from passes.  These statistics are
// printed at the end of a run, when the -stats command line option is enabled
// on the command line.
//
// This is useful for reporting information like the number of instructions
// simplified, optimized or removed by various transformations, like this:
//
// static Statistic NumInstEliminated("GCSE", "Number of instructions killed");
//
// Later, in the code: ++NumInstEliminated;
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstring>

#include "llvm/ADT/OwningPtr.h"
#include "llvm/Support/ToolOutputFile.h"
#include <sstream>
#include <ctime>

using namespace llvm;

// CreateInfoOutputFile - Return a file stream to print our output on.
namespace llvm { extern raw_ostream *CreateInfoOutputFile(); }

/// -stats - Command line option to cause transformations to emit stats about
/// what they did.
///
static cl::opt<bool>
Enabled(
    "stats",
    cl::desc("Enable statistics output from program (available with Asserts)"));

#if defined(LLVM_CSV_OUTPUT)
static cl::opt<std::string>
PrintCSV("csv", cl::desc("Print Results as CSV"),cl::value_desc("filename"));
#endif


namespace {
/// StatisticInfo - This class is used in a ManagedStatic so that it is created
/// on demand (when the first statistic is bumped) and destroyed only when
/// llvm_shutdown is called.  We print statistics from the destructor.
class StatisticInfo {
  std::vector<const Statistic*> Stats;
  friend void llvm::PrintStatistics();
  friend void llvm::PrintStatistics(raw_ostream &OS);
  friend void llvm::PrintStatisticsToCSV();
public:
  ~StatisticInfo();

  void addStatistic(const Statistic *S) {
    Stats.push_back(S);
  }
};
}

static ManagedStatic<StatisticInfo> StatInfo;
static ManagedStatic<sys::SmartMutex<true> > StatLock;

/// RegisterStatistic - The first time a statistic is bumped, this method is
/// called.
void Statistic::RegisterStatistic() {
  // If stats are enabled, inform StatInfo that this statistic should be
  // printed.
  sys::SmartScopedLock<true> Writer(*StatLock);
  if (!Initialized) {
    if (Enabled)
      StatInfo->addStatistic(this);

    TsanHappensBefore(this);
    sys::MemoryFence();
    // Remember we have been registered.
    TsanIgnoreWritesBegin();
    Initialized = true;
    TsanIgnoreWritesEnd();
  }
}

// Print information when destroyed, iff command line option is specified.
StatisticInfo::~StatisticInfo() {
  llvm::PrintStatistics();
  llvm::PrintStatisticsToCSV();
}

void llvm::EnableStatistics() {
  Enabled.setValue(true);
}

bool llvm::AreStatisticsEnabled() {
  return Enabled;
}

void llvm::PrintStatistics(raw_ostream &OS) {
  StatisticInfo &Stats = *StatInfo;

  // Figure out how long the biggest Value and Name fields are.
  unsigned MaxNameLen = 0, MaxValLen = 0;
  for (size_t i = 0, e = Stats.Stats.size(); i != e; ++i) {
    MaxValLen = std::max(MaxValLen,
                         (unsigned)utostr(Stats.Stats[i]->getValue()).size());
    MaxNameLen = std::max(MaxNameLen,
                          (unsigned)std::strlen(Stats.Stats[i]->getName()));
  }

  // Sort the fields by name.
  std::stable_sort(Stats.Stats.begin(), Stats.Stats.end(),
                   [](const Statistic *LHS, const Statistic *RHS) {
    if (int Cmp = std::strcmp(LHS->getName(), RHS->getName()))
      return Cmp < 0;

    // Secondary key is the description.
    return std::strcmp(LHS->getDesc(), RHS->getDesc()) < 0;
  });

  // Print out the statistics header...
  OS << "===" << std::string(73, '-') << "===\n"
     << "                          ... Statistics Collected ...\n"
     << "===" << std::string(73, '-') << "===\n\n";

  // Print all of the statistics.
  for (size_t i = 0, e = Stats.Stats.size(); i != e; ++i)
    OS << format("%*u %-*s - %s\n",
                 MaxValLen, Stats.Stats[i]->getValue(),
                 MaxNameLen, Stats.Stats[i]->getName(),
                 Stats.Stats[i]->getDesc());

  OS << '\n';  // Flush the output stream.
  OS.flush();

}

void llvm::PrintStatistics() {
#if !defined(NDEBUG) || defined(LLVM_ENABLE_STATS)
  StatisticInfo &Stats = *StatInfo;

  // Statistics not enabled?
  if (Stats.Stats.empty()) return;

  // Get the stream to write to.
  raw_ostream &OutStream = *CreateInfoOutputFile();
  PrintStatistics(OutStream);
  delete &OutStream;   // Close the file.
#else
  // Check if the -stats option is set instead of checking
  // !Stats.Stats.empty().  In release builds, Statistics operators
  // do nothing, so stats are never Registered.
  if (Enabled) {
    // Get the stream to write to.
    raw_ostream &OutStream = *CreateInfoOutputFile();
    OutStream << "Statistics are disabled.  "
            << "Build with asserts or with -DLLVM_ENABLE_STATS\n";
    OutStream.flush();
    delete &OutStream;   // Close the file.
  }
#endif
}

void llvm::PrintStatisticsToCSV() {
#if !defined(NDEBUG) && defined(LLVM_ENABLE_STATS) && defined(LLVM_CSV_OUTPUT)
  if (!PrintCSV.empty()) {
    std::shared_ptr<raw_ostream> OutStream = CreateInfoOutputFile();
    std::stringstream Path;
    std::string ErrorInfo;
    OwningPtr<tool_output_file> Out;

    Path << PrintCSV << ".csv";
    Out.reset(
        new tool_output_file(Path.str().c_str(), ErrorInfo, sys::fs::F_Append));

    raw_fd_ostream &Ostream = Out->os();

    *OutStream << "Writing to File " << path.str() << "\n";

    time_t Rawtime;
    struct tm *Timeinfo;
    char Timestamp[18];

    Time(&rawtime);
    Timeinfo = localtime(&Rawtime);
    int Error = strftime(Timestamp, 18, "%F-%R", Timeinfo);
    if (!Error) {
      // printing results
      StatisticInfo &Stats = *StatInfo;
      for (size_t i = 0, e = Stats.Stats.size(); i != e; ++i) {
        Ostream << PrintCSV << ",";
        Ostream << Stats.Stats[i]->getName() << ",";
        Ostream << Stats.Stats[i]->getVarName() << ",";
        Ostream << Stats.Stats[i]->getValue() << ",";
        Ostream << Stats.Stats[i]->getDesc() << ",";
        Ostream << Timestamp << "\n";
      }
    }

    OutStream->flush();
    Out->keep();
  }
#endif
}
