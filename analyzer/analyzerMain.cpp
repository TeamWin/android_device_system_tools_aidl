/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <android-base/file.h>
#include <android-base/logging.h>
#include <binder/BinderRecordReplay.h>
#include <binder/BpBinder.h>
#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
#include <signal.h>
#include <sstream>
#include "include/Analyzer.h"

using android::IBinder;
using android::NO_ERROR;
using android::sp;
using android::status_t;
using android::String16;
using android::aidl::Analyzer;
using android::binder::debug::RecordedTransaction;
using std::string;

namespace {

static volatile size_t gCtrlCCount = 0;
static constexpr size_t kCtrlCLimit = 3;
static const char kStandardRecordingPath[] = "/data/local/recordings/";

status_t startRecording(const sp<IBinder>& binder, const string& filePath) {
  if (auto mkdir_return = mkdir(kStandardRecordingPath, 0666);
      mkdir_return != 0 && errno != EEXIST) {
    std::cout << "Failed to create recordings directory.\n";
    return android::NO_ERROR;
  }

  int openFlags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_BINARY;
  android::base::unique_fd fd(open(filePath.c_str(), openFlags, 0666));
  if (fd == -1) {
    std::cout << "Failed to open file for recording with error: " << strerror(errno) << '\n';
    return android::BAD_VALUE;
  }

  // TODO (b/245804633): this still requires setenforce 0, but nothing above does
  if (status_t err = binder->remoteBinder()->startRecordingBinder(fd); err != android::NO_ERROR) {
    std::cout << "Failed to start recording with error: " << err << '\n';
    return err;
  } else {
    std::cout << "Recording started successfully.\n";
    return android::NO_ERROR;
  }
}

status_t stopRecording(const sp<IBinder>& binder) {
  if (status_t err = binder->remoteBinder()->stopRecordingBinder(); err != NO_ERROR) {
    std::cout << "Failed to stop recording with error: " << err << '\n';
    return err;
  } else {
    std::cout << "Recording stopped successfully.\n";
    return NO_ERROR;
  }
}

status_t inspectRecording(const string& interface, const string& path) {
  auto& analyzers = Analyzer::getAnalyzers();
  auto analyzer = std::find_if(
      begin(analyzers), end(analyzers),
      [&](const std::unique_ptr<Analyzer>& a) { return a->getInterfaceName() == interface; });
  if (analyzer == end(analyzers)) {
    std::cout << "Failed to find analyzer for interface: " << interface << '\n';
    return android::UNKNOWN_ERROR;
  }
  std::cout << "Found matching analyzer for interface: " << interface << '\n';

  android::base::unique_fd fd(open(path.c_str(), O_RDONLY));
  if (fd.get() == -1) {
    std::cout << "Failed to open recording file with error: " << strerror(errno) << '\n';
    return android::BAD_VALUE;
  }

  int i = 1;
  while (auto transaction = RecordedTransaction::fromFile(fd)) {
    std::cout << "Transaction " << i << ":\n";
    (*analyzer)->getAnalyzeFunction()(transaction->getCode(), transaction->getDataParcel(),
                                      transaction->getReplyParcel());

    std::cout << "Status returned from this transaction: ";
    if (transaction->getReturnedStatus() == 0) {
      std::cout << "NO_ERROR\n\n";
    } else {
      std::cout << transaction->getReturnedStatus() << "\n\n";
    }
    i++;
  }
  return NO_ERROR;
}

void incrementCtrlCCount(int signum) {
  gCtrlCCount++;
  if (gCtrlCCount > kCtrlCLimit) {
    std::cout
        << "Ctrl+C multiple times, but could not quit application. If recording still running, you "
           "might stop it manually.\n";
    exit(signum);
  }
}

status_t listenToFile(const string& filePath,
                      const std::unique_ptr<android::aidl::Analyzer>& analyzer) {
  android::base::unique_fd listenFd(open(filePath.c_str(), O_RDONLY));
  if (listenFd == -1) {
    std::cout << "Failed to open listening file with error: " << strerror(errno) << '\n';
    return android::BAD_VALUE;
  }

  signal(SIGINT, incrementCtrlCCount);
  std::cout << "Starting to listen:\n";
  int i = 1;
  while (gCtrlCCount == 0) {
    auto transaction = RecordedTransaction::fromFile(listenFd);
    if (!transaction) {
      sleep(1);
      continue;
    }
    std::cout << "Transaction " << i << ":\n";
    analyzer->getAnalyzeFunction()(transaction->getCode(), transaction->getDataParcel(),
                                   transaction->getReplyParcel());
    std::cout << "Status returned from this transaction: ";
    if (transaction->getReturnedStatus() == 0) {
      std::cout << "NO_ERROR\n\n";
    } else {
      std::cout << transaction->getReturnedStatus() << '\n\n';
    }
    i++;
  }
  return NO_ERROR;
}

status_t replayFile(const sp<IBinder>& binder, const string& interface, const string& path) {
  auto& analyzers = Analyzer::getAnalyzers();
  auto analyzer = std::find_if(
      begin(analyzers), end(analyzers),
      [&](const std::unique_ptr<Analyzer>& a) { return a->getInterfaceName() == interface; });
  if (analyzer == end(analyzers)) {
    std::cout << "Failed to find analyzer for interface: " << interface << '\n';
    return android::UNKNOWN_ERROR;
  }
  std::cout << "Found matching analyzer for interface: " << interface << '\n';

  android::base::unique_fd fd(open(path.c_str(), O_RDONLY));
  if (fd.get() == -1) {
    std::cout << "Failed to open recording file with error: " << strerror(errno) << '\n';
    return android::BAD_VALUE;
  }

  bool failure = false;
  int i = 1;
  while (auto transaction = RecordedTransaction::fromFile(fd)) {
    std::cout << "Replaying Transaction " << i << ":\n";
    (*analyzer)->getAnalyzeFunction()(transaction->getCode(), transaction->getDataParcel(),
                                      transaction->getReplyParcel());

    android::Parcel send, reply;
    send.setData(transaction->getDataParcel().data(), transaction->getDataSize());
    android::status_t status = binder->remoteBinder()->transact(transaction->getCode(), send,
                                                                &reply, transaction->getFlags());
    if (status != transaction->getReturnedStatus()) {
      std::cout << "Failure: Expected status " << transaction->getReturnedStatus()
                << " but received status " << status << "\n\n";
      failure = true;
    } else {
      std::cout << "Transaction replayed correctly."
                << "\n\n";
    }
    i++;
  }

  if (failure) {
    std::cout << "Some or all transactions failed to replay correctly. See logs for details.\n";
    return android::UNKNOWN_ERROR;
  } else {
    std::cout << "All transactions replayed correctly.\n";
    return NO_ERROR;
  }
}

status_t listAvailableInterfaces(int, char**) {
  auto& analyzers = Analyzer::getAnalyzers();
  std::cout << "Available Interfaces (" << analyzers.size() << "):\n";
  for (unsigned i = 0; i < analyzers.size(); i++) {
    std::cout << "  " << analyzers[i]->getInterfaceName() << '\n';
  }
  return NO_ERROR;
}

struct AnalyzerCommand {
  std::function<status_t(int, char*[])> command;
  std::string overview;
  std::string compactArguments;
  std::string helpDetail;
};

status_t helpCommandEntryPoint(int argc, char* argv[]);

const AnalyzerCommand helpCommand = {helpCommandEntryPoint, "Show help information.", "<command>",
                                     ""};

const AnalyzerCommand listCommand = {listAvailableInterfaces,
                                     "Prints a list of available interfaces.", "", ""};

status_t startCommandEntryPoint(int argc, char* argv[]) {
  if (argc != 3) {
    helpCommandEntryPoint(argc, argv);
    return android::BAD_VALUE;
  }

  sp<IBinder> binder = android::defaultServiceManager()->checkService(String16(argv[2]));

  string filename = argv[2];
  std::replace(filename.begin(), filename.end(), '/', '.');
  auto filePath = kStandardRecordingPath + filename;

  return startRecording(binder, filePath);
}

const AnalyzerCommand startCommand = {
    startCommandEntryPoint, "Start recording Binder transactions from a given service.",
    "<service>", "  <service>\tService to record. See 'dumpsys -l'"};

status_t stopCommandEntryPoint(int argc, char* argv[]) {
  if (argc != 3) {
    helpCommandEntryPoint(argc, argv);
    return android::BAD_VALUE;
  }

  sp<IBinder> binder = android::defaultServiceManager()->checkService(String16(argv[2]));
  return stopRecording(binder);
}

const AnalyzerCommand stopCommand = {
    stopCommandEntryPoint,
    "Stops recording Binder transactions from a given process. (See 'start')", "<service>",
    "  <service>\tService to stop recording; <service> argument to previous 'start' command."};

status_t inspectCommandEntryPoint(int argc, char* argv[]) {
  if (argc != 4) {
    helpCommandEntryPoint(argc, argv);
    return android::BAD_VALUE;
  }

  std::string interface = argv[2];
  std::string path = kStandardRecordingPath + string(argv[3]);

  return inspectRecording(interface, path);
}

const AnalyzerCommand inspectCommand = {
    inspectCommandEntryPoint,
    "Writes the binder transactions in <file-name> to stdout in a human-friendly format.",
    "<interface> <file-name>",
    "  <interface>\tA binder interface supported by this tool. (See 'list' command)\n"
    "  <file-name>\tA recording in /data/local/recordings/, and the name of the service"};

status_t listenCommandEntryPoint(int argc, char* argv[]) {
  if (argc != 4) {
    helpCommandEntryPoint(argc, argv);
    return android::BAD_VALUE;
  }

  sp<IBinder> binder = android::defaultServiceManager()->checkService(String16(argv[3]));

  string filename = argv[3];
  std::replace(filename.begin(), filename.end(), '/', '.');
  auto filePath = kStandardRecordingPath + filename;

  std::string interface = argv[2];

  auto& analyzers = Analyzer::getAnalyzers();
  auto analyzer = std::find_if(
      begin(analyzers), end(analyzers),
      [&](const std::unique_ptr<Analyzer>& a) { return a->getInterfaceName() == interface; });
  if (analyzer == end(analyzers)) {
    std::cout << "Failed to find analyzer for interface: " << interface << '\n';
    return android::UNKNOWN_ERROR;
  }

  if (status_t startErr = startRecording(binder, filePath); startErr != NO_ERROR) {
    return startErr;
  }

  status_t listenStatus = listenToFile(filePath, *analyzer);

  if (status_t stopErr = stopRecording(binder); stopErr != NO_ERROR) {
    return stopErr;
  }

  return listenStatus;
}

const AnalyzerCommand listenCommand = {
    listenCommandEntryPoint,
    "Starts recording binder transactions in <service> and writes <interface> transactions to "
    "stdout.",
    "<interface> <service>",
    "  <interface>\tA binder interface supported by this tool. (See 'list' command)"
    "  <service>\t?\n"};

int replayFunction(int argc, char* argv[]) {
  if (argc != 4) {
    return helpCommandEntryPoint(argc, argv);
  }

  sp<IBinder> binder = android::defaultServiceManager()->checkService(String16(argv[2]));
  std::string interface = argv[3];
  std::string path = kStandardRecordingPath + string(argv[4]);

  return replayFile(binder, interface, path);
}

const AnalyzerCommand replayCommand = {
    replayFunction, "No overview", "<service> <interface> <file-name>",
    "  <service>\t?\n"
    "  <interface>\tA binder interface supported by this tool. (See 'list' command)\n"
    "  <file-name>\tThe name of a file in /data/local/recordings/"};

const auto& commands = *new std::map<std::string, AnalyzerCommand>{
    {"start", startCommand},   {"stop", stopCommand},     {"inspect", inspectCommand},
    {"listen", listenCommand}, {"replay", replayCommand}, {"list", listCommand},
    {"help", helpCommand},
};

void printGeneralHelp(std::string& toolName) {
  std::cout << "USAGE: " << toolName << " <command> [<args>]\n\n";
  std::cout << "COMMANDS:\n";
  // Display overview this many characters from the start of a line.
  // Subtract the length of the command name to calculate padding.
  const size_t commandOverviewDisplayAlignment = 12;
  for (const auto& command : commands) {
    if (command.first == "help") {
      continue;
    }
    std::cout << "  " << command.first
              << std::string(commandOverviewDisplayAlignment - command.first.length(), ' ')
              << command.second.overview << "\n";
  }
  std::cout << "\n  See '" << toolName << " help <command>' for detailed help.\n";
}

status_t helpCommandEntryPoint(int argc, char* argv[]) {
  std::string toolName = argv[0];

  if (argc < 2) {
    printGeneralHelp(toolName);
    return 0;
  }

  std::string commandName = argv[1];

  if (commandName == "help") {
    if (argc < 3) {
      printGeneralHelp(toolName);
      return 0;
    }
    commandName = argv[2];
  } else {
    commandName = argv[1];
  }

  auto command = commands.find(commandName);
  if (command == commands.end()) {
    std::cout << "Unrecognized command: " << commandName;
    printGeneralHelp(toolName);
    return -1;
  }

  std::cout << "OVERVIEW: " << command->second.overview << "\n\n";
  std::cout << "USAGE: " << toolName << " " << commandName << " "
            << command->second.compactArguments << "\n\n";
  std::cout << "ARGUMENTS:\n" << command->second.helpDetail << "\n";

  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string toolName = argv[0];

  if (argc < 2 ||
      (argc >= 2 && ((strcmp(argv[1], "--help") == 0) || (strcmp(argv[1], "-h") == 0)))) {
    // General help
    printGeneralHelp(toolName);
    return 0;
  }

  auto command = commands.find(argv[1]);
  if (command == commands.end()) {
    std::cout << "Unrecognized command: " << argv[1] << "\n";
    printGeneralHelp(toolName);
    return -1;
  }

  return command->second.command(argc, argv);
}
