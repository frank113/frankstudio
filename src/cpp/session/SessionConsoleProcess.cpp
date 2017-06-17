/*
 * SessionConsoleProcess.cpp
 *
 * Copyright (C) 2009-17 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include <session/SessionConsoleProcess.hpp>

#include <core/Algorithm.hpp>

#include <session/SessionModuleContext.hpp>

#include "modules/SessionWorkbench.hpp"
#include "SessionConsoleProcessTable.hpp"

using namespace rstudio::core;

namespace rstudio {
namespace session {
namespace console_process {

namespace {

ConsoleProcessSocket s_terminalSocket;

// minimum delay between private command executions
const boost::posix_time::milliseconds kPrivateCommandDelay = boost::posix_time::milliseconds(3000);

// how long after a command is started do we delay before considering running a private command
const boost::posix_time::milliseconds kWaitForCommandDelay = boost::posix_time::milliseconds(1500);

boost::posix_time::ptime now()
{
   return boost::posix_time::microsec_clock::universal_time();
}

bool useWebsockets()
{
   return session::options().allowTerminalWebsockets() &&
                     session::userSettings().terminalWebsockets();
}

} // anonymous namespace

// create process options for a terminal
core::system::ProcessOptions ConsoleProcess::createTerminalProcOptions(
      TerminalShell::TerminalShellType desiredShellType,
      int cols, int rows, int termSequence,
      FilePath workingDir,
      TerminalShell::TerminalShellType* pSelectedShellType)
{
   // configure environment for shell
   core::system::Options shellEnv;
   core::system::environment(&shellEnv);

   *pSelectedShellType = desiredShellType;

#ifndef _WIN32
   // set xterm title to show current working directory after each command
   core::system::setenv(&shellEnv, "PROMPT_COMMAND",
                        "echo -ne \"\\033]0;${PWD/#${HOME}/~}\\007\"");

   std::string editorCommand = session::modules::workbench::editFileCommand();
   if (!editorCommand.empty())
   {
      core::system::setenv(&shellEnv, "GIT_EDITOR", editorCommand);
      core::system::setenv(&shellEnv, "SVN_EDITOR", editorCommand);
   }
#endif

   if (termSequence != kNoTerminal)
   {
      core::system::setenv(&shellEnv, "RSTUDIO_TERM",
                           boost::lexical_cast<std::string>(termSequence));
   }

   // ammend shell paths as appropriate
   session::modules::workbench::ammendShellPaths(&shellEnv);

   // set options
   core::system::ProcessOptions options;
   options.workingDir = workingDir.empty() ? module_context::shellWorkingDirectory() :
                                             workingDir;
   options.environment = shellEnv;
   options.smartTerminal = true;
   options.reportHasSubprocs = true;
   options.trackCwd = true;
   options.cols = cols;
   options.rows = rows;

   // set path to shell
   AvailableTerminalShells shells;
   TerminalShell shell;

   if (shells.getInfo(desiredShellType, &shell))
   {
      *pSelectedShellType = shell.type;
      options.shellPath = shell.path;
      options.args = shell.args;
   }

   // last-ditch, use system shell
   if (!options.shellPath.exists())
   {
      TerminalShell sysShell;
      if (AvailableTerminalShells::getSystemShell(&sysShell))
      {
         *pSelectedShellType = sysShell.type;
         options.shellPath = sysShell.path;
         options.args = sysShell.args;
      }
   }

   return options;
}

ConsoleProcess::ConsoleProcess(boost::shared_ptr<ConsoleProcessInfo> procInfo)
   : procInfo_(procInfo), interrupt_(false), interruptChild_(false),
     newCols_(-1), newRows_(-1), pid_(-1), childProcsSent_(false),
     lastInputSequence_(kIgnoreSequence), started_(false), haveProcOps_(false),
     privateCommandLoop_(false),
     lastPrivateCommand_(boost::posix_time::not_a_date_time),
     lastEnterTime_(boost::posix_time::not_a_date_time),
     pendingCommand_(true)
{
   regexInit();

   // When we retrieve from outputBuffer, we only want complete lines. Add a
   // dummy \n so we can tell the first line is a complete line.
   procInfo_->appendToOutputBuffer('\n');
}

ConsoleProcess::ConsoleProcess(const std::string& command,
                               const core::system::ProcessOptions& options,
                               boost::shared_ptr<ConsoleProcessInfo> procInfo)
   : command_(command), options_(options), procInfo_(procInfo),
     interrupt_(false), interruptChild_(false), newCols_(-1), newRows_(-1),
     pid_(-1), childProcsSent_(false), lastInputSequence_(kIgnoreSequence),
     started_(false), haveProcOps_(false), privateCommandLoop_(false),
     lastPrivateCommand_(boost::posix_time::not_a_date_time),
     lastEnterTime_(boost::posix_time::not_a_date_time),
     pendingCommand_(true)

{
   commonInit();
}

ConsoleProcess::ConsoleProcess(const std::string& program,
                               const std::vector<std::string>& args,
                               const core::system::ProcessOptions& options,
                               boost::shared_ptr<ConsoleProcessInfo> procInfo)
   : program_(program), args_(args), options_(options), procInfo_(procInfo),
     interrupt_(false), interruptChild_(false), newCols_(-1), newRows_(-1),
     pid_(-1), childProcsSent_(false), lastInputSequence_(kIgnoreSequence),
     started_(false), haveProcOps_(false), privateCommandLoop_(false),
     lastPrivateCommand_(boost::posix_time::not_a_date_time),
     lastEnterTime_(boost::posix_time::not_a_date_time),
     pendingCommand_(true)
{
   commonInit();
}

void ConsoleProcess::regexInit()
{
   controlCharsPattern_ = boost::regex("[\\r\\b]");
   promptPattern_ = boost::regex("^(.+)[\\W_]( +)$");
}

void ConsoleProcess::commonInit()
{
   regexInit();
   procInfo_->ensureHandle();

   privateOutputBOM_ = core::system::generateUuid(false);
   privateOutputEOM_ = core::system::generateUuid(true);

   captureEnvironmentCommand_ =  "echo ";
   captureEnvironmentCommand_ += privateOutputBOM_;
   captureEnvironmentCommand_ += "\n/usr/bin/env && echo ";
   captureEnvironmentCommand_ += privateOutputEOM_;
   captureEnvironmentCommand_ += "\n";

   // always redirect stderr to stdout so output is interleaved
   options_.redirectStdErrToStdOut = true;

   if (interactionMode() != InteractionNever)
   {
#ifdef _WIN32
      // NOTE: We use consoleio.exe here in order to make sure svn.exe password
      // prompting works properly

      FilePath consoleIoPath = session::options().consoleIoPath();

      // if this is as runProgram then fixup the program and args
      if (!program_.empty())
      {
         options_.createNewConsole = true;

         // build new args
         shell_utils::ShellArgs args;
         args << program_;
         args << args_;

         // fixup program_ and args_ so we run the consoleio.exe proxy
         program_ = consoleIoPath.absolutePathNative();
         args_ = args;
      }
      // if this is a runCommand then prepend consoleio.exe to the command
      else if (!command_.empty())
      {
         options_.createNewConsole = true;
         command_ = shell_utils::escape(consoleIoPath) + " " + command_;
      }
      else // terminal
      {
         // undefine TERM, as it puts git-bash in a mode that winpty doesn't
         // support; was set in SessionMain.cpp::main to support color in
         // the R Console
         if (!options_.environment)
         {
            core::system::Options childEnv;
            core::system::environment(&childEnv);
            options_.environment = childEnv;
         }
         core::system::unsetenv(&(options_.environment.get()), "TERM");

         // request a pseudoterminal if this is an interactive console process
         options_.pseudoterminal = core::system::Pseudoterminal(
                  session::options().winptyPath(),
                  false /*plainText*/,
                  false /*conerr*/,
                  options_.cols,
                  options_.rows);
      }
#else
      // request a pseudoterminal if this is an interactive console process
      options_.pseudoterminal = core::system::Pseudoterminal(options_.cols,
                                                             options_.rows);

      // define TERM (but first make sure we have an environment
      // block to modify)
      if (!options_.environment)
      {
         core::system::Options childEnv;
         core::system::environment(&childEnv);
         options_.environment = childEnv;
      }

      core::system::setenv(&(options_.environment.get()), "TERM",
                           options_.smartTerminal ? core::system::kSmartTerm :
                                                    core::system::kDumbTerm);
#endif
   }

   // When we retrieve from outputBuffer, we only want complete lines. Add a
   // dummy \n so we can tell the first line is a complete line.
   if (!options_.smartTerminal)
      procInfo_->appendToOutputBuffer('\n');
}

std::string ConsoleProcess::bufferedOutput() const
{
   if (options_.smartTerminal)
      return "";

   return procInfo_->bufferedOutput();
}

void ConsoleProcess::setPromptHandler(
      const boost::function<bool(const std::string&, Input*)>& onPrompt)
{
   onPrompt_ = onPrompt;
}

Error ConsoleProcess::start()
{
   if (started_ || procInfo_->getZombie())
      return Success();

   Error error;
   if (!command_.empty())
   {
      error = module_context::processSupervisor().runCommand(
                                 command_, options_, createProcessCallbacks());
   }
   else if (!program_.empty())
   {
      error = module_context::processSupervisor().runProgram(
                          program_, args_, options_, createProcessCallbacks());
   }
   else
   {
      error = module_context::processSupervisor().runTerminal(
                          options_, createProcessCallbacks());
   }
   if (!error)
      started_ = true;
   return error;
}

void ConsoleProcess::enqueInput(const Input& input)
{
   LOCK_MUTEX(inputQueueMutex_)
   {
      if (input.sequence == kIgnoreSequence)
      {
         inputQueue_.push_back(input);
         return;
      }

      if (input.sequence == kFlushSequence)
      {
         inputQueue_.push_back(input);

         // set everything in queue to "ignore" so it will be pulled from
         // queue as-is, even with gaps
         for (std::deque<Input>::iterator it = inputQueue_.begin();
              it != inputQueue_.end(); it++)
         {
            (*it).sequence = kIgnoreSequence;
         }
         lastInputSequence_ = kIgnoreSequence;
         return;
      }

      // insert in order by sequence
      for (std::deque<Input>::iterator it = inputQueue_.begin();
           it != inputQueue_.end(); it++)
      {
         if (input.sequence < (*it).sequence)
         {
            inputQueue_.insert(it, input);
            return;
         }
      }
      inputQueue_.push_back(input);
   }
   END_LOCK_MUTEX
}

// Do not call directly, needs to be within the inputQueueMutex_ for safety in
// presence of multithreaded calls (via websockets). This is handled by
// processQueuedInput.
ConsoleProcess::Input ConsoleProcess::dequeInput()
{
   // Pull next available Input from queue; return an empty Input
   // if none available or if an out-of-sequence entry is
   // reached; assumption is the missing item(s) will eventually
   // arrive and unblock the backlog.
   if (inputQueue_.empty())
      return Input();

   Input input = inputQueue_.front();
   if (input.sequence == kIgnoreSequence || input.sequence == kFlushSequence)
   {
      inputQueue_.pop_front();
      return input;
   }

   if (input.sequence == lastInputSequence_ + 1)
   {
      lastInputSequence_++;
      inputQueue_.pop_front();
      return input;
   }

   // Getting here means input is out of sequence. We want to prevent
   // getting permanently stuck if a message gets lost and the
   // gap(s) never get filled in. So we'll flush it if input piles up.
   if (inputQueue_.size() >= kAutoFlushLength)
   {
      // set everything in queue to "ignore" so it will be pulled from
      // queue as-is, even with gaps
      for (std::deque<Input>::iterator it = inputQueue_.begin();
           it != inputQueue_.end(); it++)
      {
         lastInputSequence_ = (*it).sequence;
         (*it).sequence = kIgnoreSequence;
      }

      input.sequence = kIgnoreSequence;
      inputQueue_.pop_front();
      return input;
   }

   return Input();
}

void ConsoleProcess::enquePromptEvent(const std::string& prompt)
{
   // enque a prompt event
   json::Object data;
   data["handle"] = handle();
   data["prompt"] = prompt;
   module_context::enqueClientEvent(
         ClientEvent(client_events::kConsoleProcessPrompt, data));
}

void ConsoleProcess::enquePrompt(const std::string& prompt)
{
   enquePromptEvent(prompt);
}

void ConsoleProcess::interrupt()
{
   interrupt_ = true;
}

void ConsoleProcess::interruptChild()
{
   interruptChild_ = true;
}

void ConsoleProcess::resize(int cols, int rows)
{
   newCols_ = cols;
   newRows_ = rows;
}

bool ConsoleProcess::privateCommandLoop(core::system::ProcessOperations& ops)
{
   if (!procInfo_->getTrackEnv() || procInfo_->getHasChildProcs())
      return false;

   boost::posix_time::ptime currentTime = now();
   if (privateCommandLoop_.get())
   {
      // TODO (gary)
      // safeguard timeout here to exit private command loop if parsing fails after
      // a couple of seconds!

      return true;
   }
   else
   {
      LOCK_MUTEX(inputQueueMutex_)
      {
         // We don't start a private command if something is being typed, or a command has never
         // been run.
         if (pendingCommand_ || lastEnterTime_.is_not_a_date_time())
            return false;

         if (currentTime - kWaitForCommandDelay <= lastEnterTime_)
         {
            // not enough time has elapsed since last command was submitted
            return false;
         }

         if (!lastPrivateCommand_.is_not_a_date_time() &&
             currentTime - kPrivateCommandDelay <= lastPrivateCommand_)
         {
            // not enough time has elapsed since last private command ran
            return false;
         }

         if (!lastPrivateCommand_.is_not_a_date_time() &&
             lastPrivateCommand_ > lastEnterTime_)
         {
            // Hasn't been a new command executed since our last private command, no need
            // to run it.
            return false;
         }
      }
      END_LOCK_MUTEX

      lastPrivateCommand_ = currentTime;
      privateCommandLoop_.set(true);

      // send the command
      Error error = ops.writeToStdin(captureEnvironmentCommand_, false);
      if (error)
      {
         LOG_ERROR(error);
         privateCommandLoop_.set(false);
         lastPrivateCommand_ = boost::posix_time::pos_infin; // disable private commands
         return false;
      }
      return true;
   }
   return false;
}

bool ConsoleProcess::onContinue(core::system::ProcessOperations& ops)
{
   // full stop interrupt if requested
   if (interrupt_)
      return false;

   // send SIGINT to children of the shell
   if (interruptChild_)
   {
      Error error = ops.ptyInterrupt();
      if (error)
         LOG_ERROR(error);
      interruptChild_ = false;
   }

   // opportunity to execute a private commmand (send a command-line to the shell and
   // capture the output for special processing, but end-user doesn't see it)
   if (privateCommandLoop(ops))
      return true;

   // For RPC-based communication, this is where input is always dispatched; for websocket
   // communication, it is normally dispatched inside onReceivedInput, but this call is needed
   // to deal with input built-up during a privateCommandLoop.
   processQueuedInput(ops);

   if (procInfo_->getChannelMode() == Websocket)
   {
      // capture weak reference to the callbacks so websocket callback
      // can use them; only need to capture the first time
      if (!haveProcOps_)
      {
         LOCK_MUTEX(procOpsMutex_)
         {
            pOps_ = ops.weak_from_this();
            haveProcOps_ = true;
         }
         END_LOCK_MUTEX
      }
   }

   if (newCols_ != -1 && newRows_ != -1)
   {
      ops.ptySetSize(newCols_, newRows_);
      procInfo_->setCols(newCols_);
      procInfo_->setRows(newRows_);
      newCols_ = -1;
      newRows_ = -1;
      saveConsoleProcesses();
   }

   pid_ = ops.getPid();

   // continue
   return true;
}

void ConsoleProcess::processQueuedInput(core::system::ProcessOperations& ops)
{
   LOCK_MUTEX(inputQueueMutex_)
   {
      // process input queue
      Input input = dequeInput();
      while (!input.empty())
      {
         pendingCommand_ = true;

         // pty interrupt
         if (input.interrupt)
         {
            Error error = ops.ptyInterrupt();
            if (error)
               LOG_ERROR(error);

            if (input.echoInput)
               procInfo_->appendToOutputBuffer("^C");
         }

         // text input
         else
         {
            std::string inputText = input.text;

            if (!inputText.empty() && *inputText.rbegin() == '\r')
            {
               lastEnterTime_ = now();
               pendingCommand_ = false;
            }

#ifdef _WIN32
            if (!options_.smartTerminal)
            {
               string_utils::convertLineEndings(&inputText, string_utils::LineEndingWindows);
            }
#endif
            Error error = ops.writeToStdin(inputText, false);
            if (error)
               LOG_ERROR(error);

            if (!options_.smartTerminal) // smart terminal does echo via pty
            {
               if (input.echoInput)
                  procInfo_->appendToOutputBuffer(inputText);
               else
                  procInfo_->appendToOutputBuffer("\n");
            }
         }

         input = dequeInput();
      }
   }
   END_LOCK_MUTEX
}

void ConsoleProcess::deleteLogFile(bool lastLineOnly) const
{
   procInfo_->deleteLogFile(lastLineOnly);
}

std::string ConsoleProcess::getSavedBufferChunk(int chunk, bool* pMoreAvailable) const
{
   return procInfo_->getSavedBufferChunk(chunk, pMoreAvailable);
}

std::string ConsoleProcess::getBuffer() const
{
   return procInfo_->getFullSavedBuffer();
}

void ConsoleProcess::enqueOutputEvent(const std::string &output)
{
   if (privateCommandLoop_.get())
   {
// TODO (gary)
// capture results of a private command
//      return;
      privateCommandLoop_.set(false);
   }

   // normal output processing
   bool currentAltBufferStatus = procInfo_->getAltBufferActive();

   // copy to output buffer
   procInfo_->appendToOutputBuffer(output);

   if (procInfo_->getAltBufferActive() != currentAltBufferStatus)
      saveConsoleProcesses();

   // If there's more output than the client can even show, then
   // truncate it to the amount that the client can show. Too much
   // output can overwhelm the client, making it unresponsive.
   std::string trimmedOutput = output;
   string_utils::trimLeadingLines(procInfo_->getMaxOutputLines(), &trimmedOutput);

   if (procInfo_->getChannelMode() == Websocket)
   {
      s_terminalSocket.sendText(procInfo_->getHandle(), output);
      return;
   }

   // Rpc
   json::Object data;
   data["handle"] = handle();
   data["output"] = trimmedOutput;
   module_context::enqueClientEvent(
         ClientEvent(client_events::kConsoleProcessOutput, data));
}

void ConsoleProcess::onStdout(core::system::ProcessOperations& ops,
                              const std::string& output)
{
   if (options_.smartTerminal)
   {
      enqueOutputEvent(output);
      return;
   }

   // convert line endings to posix
   std::string posixOutput = output;
   string_utils::convertLineEndings(&posixOutput,
                                    string_utils::LineEndingPosix);

   // process as normal output or detect a prompt if there is one
   if (boost::algorithm::ends_with(posixOutput, "\n"))
   {
      enqueOutputEvent(posixOutput);
   }
   else
   {
      // look for the last newline and take the content after
      // that as the prompt
      std::size_t lastLoc = posixOutput.find_last_of("\n\f");
      if (lastLoc != std::string::npos)
      {
         enqueOutputEvent(posixOutput.substr(0, lastLoc));
         maybeConsolePrompt(ops, posixOutput.substr(lastLoc + 1));
      }
      else
      {
         maybeConsolePrompt(ops, posixOutput);
      }
   }
}

void ConsoleProcess::maybeConsolePrompt(core::system::ProcessOperations& ops,
                                        const std::string& output)
{
   boost::smatch smatch;

   // treat special control characters as output rather than a prompt
   if (regex_utils::search(output, smatch, controlCharsPattern_))
      enqueOutputEvent(output);

   // make sure the output matches our prompt pattern
   if (!regex_utils::match(output, smatch, promptPattern_))
      enqueOutputEvent(output);

   // it is a prompt
   else
      handleConsolePrompt(ops, output);
}

void ConsoleProcess::handleConsolePrompt(core::system::ProcessOperations& ops,
                                         const std::string& prompt)
{
   // if there is a custom prompt handler then give it a chance to
   // handle the prompt first
   if (onPrompt_)
   {
      Input input;
      if (onPrompt_(prompt, &input))
      {
         if (!input.empty())
         {
            enqueInput(input);
         }
         else
         {
            Error error = ops.terminate();
            if (error)
              LOG_ERROR(error);
         }

         return;
      }
   }

   enquePromptEvent(prompt);
}

void ConsoleProcess::onExit(int exitCode)
{
   procInfo_->setExitCode(exitCode);
   procInfo_->setHasChildProcs(false);

   saveConsoleProcesses();

   json::Object data;
   data["handle"] = handle();
   data["exitCode"] = exitCode;
   module_context::enqueClientEvent(
         ClientEvent(client_events::kConsoleProcessExit, data));

   onExit_(exitCode);
}

void ConsoleProcess::onHasSubprocs(bool hasSubprocs)
{
   if (hasSubprocs != procInfo_->getHasChildProcs() || !childProcsSent_)
   {
      procInfo_->setHasChildProcs(hasSubprocs);

      json::Object subProcs;
      subProcs["handle"] = handle();
      subProcs["subprocs"] = procInfo_->getHasChildProcs();
      module_context::enqueClientEvent(
            ClientEvent(client_events::kTerminalSubprocs, subProcs));
      childProcsSent_ = true;
   }
}

void ConsoleProcess::reportCwd(const core::FilePath& cwd)
{
   if (procInfo_->getCwd() != cwd)
   {
      procInfo_->setCwd(cwd);

      json::Object termCwd;
      termCwd["handle"] = handle();
      termCwd["cwd"] = module_context::createAliasedPath(cwd);
      module_context::enqueClientEvent(
            ClientEvent(client_events::kTerminalCwd, termCwd));
      childProcsSent_ = true;

      saveConsoleProcesses();
   }
}

std::string ConsoleProcess::getChannelMode() const
{
   switch(procInfo_->getChannelMode())
   {
   case Rpc:
      return "rpc";
   case Websocket:
      return "websocket";
   default:
      return "unknown";
   }
}

void ConsoleProcess::setRpcMode()
{
   s_terminalSocket.stopListening(handle());
   procInfo_->setChannelMode(Rpc, "");
}

void ConsoleProcess::setZombie()
{
   procInfo_->setZombie(true);
   procInfo_->setHasChildProcs(false);
   saveConsoleProcesses();
}

core::json::Object ConsoleProcess::toJson() const
{
   return procInfo_->toJson();
}

ConsoleProcessPtr ConsoleProcess::fromJson(
                                             core::json::Object &obj)
{
   boost::shared_ptr<ConsoleProcessInfo> pProcInfo(ConsoleProcessInfo::fromJson(obj));
   ConsoleProcessPtr pProc(new ConsoleProcess(pProcInfo));
   return pProc;
}

core::system::ProcessCallbacks ConsoleProcess::createProcessCallbacks()
{
   core::system::ProcessCallbacks cb;
   cb.onContinue = boost::bind(&ConsoleProcess::onContinue, ConsoleProcess::shared_from_this(), _1);
   cb.onStdout = boost::bind(&ConsoleProcess::onStdout, ConsoleProcess::shared_from_this(), _1, _2);
   cb.onExit = boost::bind(&ConsoleProcess::onExit, ConsoleProcess::shared_from_this(), _1);
   if (options_.reportHasSubprocs)
   {
      cb.onHasSubprocs = boost::bind(&ConsoleProcess::onHasSubprocs, ConsoleProcess::shared_from_this(), _1);
   }
   if (options_.trackCwd)
   {
      cb.reportCwd = boost::bind(&ConsoleProcess::reportCwd, ConsoleProcess::shared_from_this(), _1);
   }
   return cb;
}

ConsoleProcessPtr ConsoleProcess::create(
      const std::string& command,
      core::system::ProcessOptions options,
      boost::shared_ptr<ConsoleProcessInfo> procInfo)
{
   options.terminateChildren = true;
   ConsoleProcessPtr ptrProc(
         new ConsoleProcess(command, options, procInfo));
   addConsoleProcess(ptrProc);
   saveConsoleProcesses();
   return ptrProc;
}

ConsoleProcessPtr ConsoleProcess::create(
      const std::string& program,
      const std::vector<std::string>& args,
      core::system::ProcessOptions options,
      boost::shared_ptr<ConsoleProcessInfo> procInfo)
{
   options.terminateChildren = true;
   ConsoleProcessPtr ptrProc(
         new ConsoleProcess(program, args, options, procInfo));
   addConsoleProcess(ptrProc);
   saveConsoleProcesses();
   return ptrProc;
}

// supports reattaching to a running process, or creating a new process with
// previously used handle
ConsoleProcessPtr ConsoleProcess::createTerminalProcess(
      core::system::ProcessOptions options,
      boost::shared_ptr<ConsoleProcessInfo> procInfo,
      bool enableWebsockets)
{
   ConsoleProcessPtr cp;
   procInfo->setRestarted(true); // only flip to false if we find an existing
                                 // process for this terminal handle

   // Use websocket as preferred communication channel; it can fail
   // here if unable to establish the server-side of things, in which case
   // fallback to using Rpc.
   //
   // It can also fail later when client tries to connect; fallback for that
   // happens from the client-side via RPC call procUseRpc.
   if (enableWebsockets)
   {
      Error error = s_terminalSocket.ensureServerRunning();
      if (error)
      {
         procInfo->setChannelMode(Rpc, "");
         LOG_ERROR(error);
      }
      else
      {
         std::string port = safe_convert::numberToString(s_terminalSocket.port());
         procInfo->setChannelMode(Websocket, port);
      }
   }
   else
   {
      procInfo->setChannelMode(Rpc, "");
   }

   std::string command;
   if (procInfo->getAllowRestart() && !procInfo->getHandle().empty())
   {
      // return existing ConsoleProcess if it is still running
      ConsoleProcessPtr proc = findProcByHandle(procInfo->getHandle());
      if (proc != NULL && proc->isStarted())
      {
         cp = proc;
         cp->procInfo_->setRestarted(false);

         if (proc->procInfo_->getAltBufferActive())
         {
            // Jiggle the size of the pseudo-terminal, this will force the app
            // to refresh itself; this does rely on the host performing a second
            // resize to the actual available size. Clumsy, but so far this is
            // the best I've come up with.
            cp->resize(core::system::kDefaultCols / 2, core::system::kDefaultRows / 2);
         }
      }
      else
      {
         // Create new process with previously used handle

         // previous terminal session might have been killed while a full-screen
         // program was running
         procInfo->setAltBufferActive(false);

         options.terminateChildren = true;
         cp.reset(new ConsoleProcess(command, options, procInfo));
         addConsoleProcess(cp);

         // Windows Command Prompt and PowerShell don't support reloading
         // buffers, so delete the buffer before we start the new process.
         if (cp->getShellType() == TerminalShell::Cmd32 ||
             cp->getShellType() == TerminalShell::Cmd64 ||
             cp->getShellType() == TerminalShell::PS32 ||
             cp->getShellType() == TerminalShell::PS64)
         {
            cp->deleteLogFile();
         }

         saveConsoleProcesses();
      }
   }
   else
   {
      // otherwise create a new one
      cp =  create(command, options, procInfo);
   }

   if (cp->procInfo_->getChannelMode() == Websocket)
   {
      // start watching for websocket callbacks
      s_terminalSocket.listen(cp->procInfo_->getHandle(),
                              cp->createConsoleProcessSocketConnectionCallbacks());
   }
   return cp;
}

ConsoleProcessPtr ConsoleProcess::createTerminalProcess(
      core::system::ProcessOptions options,
      boost::shared_ptr<ConsoleProcessInfo> procInfo)
{
   return createTerminalProcess(options, procInfo, useWebsockets());
}


ConsoleProcessPtr ConsoleProcess::createTerminalProcess(
      ConsoleProcessPtr proc)
{
   TerminalShell::TerminalShellType actualShellType;
   core::system::ProcessOptions options = ConsoleProcess::createTerminalProcOptions(
            proc->procInfo_->getShellType(),
            proc->procInfo_->getCols(), proc->procInfo_->getRows(),
            proc->procInfo_->getTerminalSequence(),
            proc->procInfo_->getCwd(),
            &actualShellType);
   proc->procInfo_->setShellType(actualShellType);
   return createTerminalProcess(options, proc->procInfo_);
}

ConsoleProcessSocketConnectionCallbacks ConsoleProcess::createConsoleProcessSocketConnectionCallbacks()
{
   ConsoleProcessSocketConnectionCallbacks cb;
   cb.onReceivedInput = boost::bind(&ConsoleProcess::onReceivedInput, ConsoleProcess::shared_from_this(), _1);
   cb.onConnectionOpened = boost::bind(&ConsoleProcess::onConnectionOpened, ConsoleProcess::shared_from_this());
   cb.onConnectionClosed = boost::bind(&ConsoleProcess::onConnectionClosed, ConsoleProcess::shared_from_this());
   return cb;
}

// received input from websocket (e.g. user typing on client), or from
// rstudioapi, may be called on different thread
void ConsoleProcess::onReceivedInput(const std::string& input)
{
   enqueInput(Input(input));
   LOCK_MUTEX(procOpsMutex_)
   {
      boost::shared_ptr<core::system::ProcessOperations> ops = pOps_.lock();
      if (ops)
      {
         if (!privateCommandLoop_.get())
            processQueuedInput(*ops);
      }
   }
   END_LOCK_MUTEX
}

// websocket connection closed; called on different thread
void ConsoleProcess::onConnectionClosed()
{
   s_terminalSocket.stopListening(handle());
}

// websocket connection opened; called on different thread
void ConsoleProcess::onConnectionOpened()
{
}

std::string ConsoleProcess::getShellName() const
{
   return TerminalShell::getShellName(procInfo_->getShellType());
}

core::json::Array processesAsJson()
{
   return allProcessesAsJson();
}

Error initialize()
{
   return internalInitialize();
}

} // namespace console_process
} // namespace session
} // namespace rstudio
