/* 
	Copyright 2010 OpenRTMFP

	This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License received along this program for more
	details (or else see http://www.gnu.org/licenses/).

	This file is a part of Cumulus.
*/

#include "RTMFPServer.h"
#include "Logs.h"
#include "Auth.h"
#include "Server.h"
#include "ApplicationKiller.h"
#include "Poco/File.h"
#include "Poco/Format.h"
#include "Poco/FileStream.h"
#include "Poco/DateTimeFormatter.h"
#include "Poco/Util/HelpFormatter.h"
#include "Poco/Util/ServerApplication.h"
#include "string.h"
#include <iostream>

#define LOG_FILE(END)		"/log."#END
#define LOG_FILE_VAR(NUM)	format("/log.%d",NUM)

#define LOG_SIZE 1000000

using namespace std;
using namespace Poco;
using namespace Poco::Net;
using namespace Poco::Util;
using namespace Cumulus;

const char * g_logPriorities[] = { "FATAL","CRITIC" ,"ERROR","WARN","NOTE","INFO","DEBUG","TRACE" };

class CumulusService: public ServerApplication , private Cumulus::Logger, private ApplicationKiller  {
public:
	CumulusService(): _helpRequested(false),_pCirrus(NULL),_middle(false),_isInteractive(true),_pLogFile(NULL) {
		
	}
	
	~CumulusService() {
		if(_pCirrus)
			delete _pCirrus;
		if(_pLogFile)
			delete _pLogFile;
	}

private:

	void kill() {
		terminate();
	}

	void initialize(Application& self) {
		loadConfiguration(); // load default configuration files, if present
		ServerApplication::initialize(self);
		_isInteractive = isInteractive();
		_logDir = config().getString("logs.dir",config().getString("application.dir",".")+"/logs");
		_pLogFile = new File(_logDir+LOG_FILE(0));
		File(_logDir).createDirectory();
		_logStream.open(_pLogFile->path(),ios::in | ios::ate);
		Logs::SetLogger(*this);
		_auth.authIsWhitelist=config().getBool("auth.whitelist",false);
		_auth.load("auth");
	}
		
	void uninitialize() {
		ServerApplication::uninitialize();
	}

	void defineOptions(OptionSet& options) {
		ServerApplication::defineOptions(options);

		options.addOption(
			Option("log", "l", "Log level argument, must be beetween 0 and 8 : nothing, fatal, critic, error, warn, note, info, debug, trace. Default value is 6 (info), all logs until info level are displayed.")
				.required(false)
				.argument("level")
				.repeatable(false));

		options.addOption(
			Option("dump", "d", "Enables packet traces in logs. Optional arguments are 'middle' or 'all' respectively to displays just middle packet process or all packet process. If no argument is given, just outside packet process will be dumped.",false,"middle|all",false)
				.repeatable(false));

		options.addOption(
			Option("cirrus", "c", "Cirrus address to activate a 'man-in-the-middle' developer mode in bypassing flash packets to the official cirrus server of your choice, it's a instable mode to help Cumulus developers, \"p2p.rtmfp.net:10000\" for example. By adding the 'dump' argument, you will able to display Cirrus/Flash packet exchange in your logs (see 'dump' argument).",false,"address",true)
				.repeatable(false));

		options.addOption(
			Option("middle", "m","Enables a 'man-in-the-middle' developer mode between two peers. It's a instable mode to help Cumulus developers. By adding the 'dump' argument, you will able to display Flash/Flash packet exchange in your logs (see 'dump' argument).")
				.repeatable(false));

		options.addOption(
			Option("help", "h", "Displays help information about command-line usage.")
				.required(false)
				.repeatable(false));
	}

	void handleOption(const std::string& name, const std::string& value) {
		ServerApplication::handleOption(name, value);

		if (name == "help")
			_helpRequested = true;
		else if (name == "cirrus") {
			try {
				URI uri("rtmfp://"+value);
				_pCirrus = new SocketAddress(uri.getHost(),uri.getPort());
				NOTE("Mode 'man in the middle' : the exchange will bypass to '%s'",value.c_str());
			} catch(Exception& ex) {
				ERROR("Mode 'man in the middle' error : %s",ex.displayText().c_str());
			}
		} else if (name == "dump") {
			if(value == "all")
				Logs::SetDump(Logs::ALL);
			else if(value == "middle")
				Logs::SetDump(Logs::MIDDLE);
			else
				Logs::SetDump(Logs::EXTERNAL);
		} else if (name == "middle")
			_middle = true;
		else if (name == "log")
			Logs::SetLevel(atoi(value.c_str()));
	}

	void displayHelp() {
		HelpFormatter helpFormatter(options());
		helpFormatter.setCommand(commandName());
		helpFormatter.setUsage("OPTIONS");
		helpFormatter.setHeader("Cumulus 'rendezvous' server for RTMFP communication");
		helpFormatter.format(cout);
	}

	void dumpHandler(const char* data,int size) {
		cout.write(data,size);
		_logStream.write(data,size);
		manageLogFile();
	}

	void logHandler(Thread::TID threadId,const std::string& threadName,Priority priority,const char *filePath,long line, const char *text) {
		ScopedLock<FastMutex> lock(_logMutex);
		if(_isInteractive)
			printf("%s  %s[%ld] %s\n",g_logPriorities[priority-1],Path(filePath).getBaseName().c_str(),line,text);
		_logStream << DateTimeFormatter::format(LocalDateTime(),"%d/%m %H:%M:%S.%c  ")
				<< g_logPriorities[priority-1] << '\t' << threadName << '(' << threadId << ")\t"
				<< Path(filePath).getFileName() << '[' << line << "]  " << text << std::endl;
		manageLogFile();
	}

	void manageLogFile() {
		if(_pLogFile->getSize()>LOG_SIZE) {
			_logStream.close();
			int num = 10;
			File file(_logDir+LOG_FILE(10));
			if(file.exists())
				file.remove();
			while(--num>=0) {
				file = _logDir+LOG_FILE_VAR(num);
				if(file.exists())
					file.renameTo(_logDir+LOG_FILE_VAR(num+1));
			}
			_logStream.open(_pLogFile->path(),ios::in | ios::ate);
		}	
	}


///// MAIN
	int main(const std::vector<std::string>& args) {
		if (_helpRequested) {
			displayHelp();
		}
		else {
			// starts the server
			RTMFPServerParams params;
			params.port = config().getInt("port", RTMFP_DEFAULT_PORT);
			params.pCirrus = _pCirrus;
			params.middle = _middle;
			params.keepAliveServer = config().getInt("keepAliveServer",15);
			params.keepAlivePeer = config().getInt("keepAlivePeer",10);
			Server server(_auth,*this);
			server.start(params);

			// wait for CTRL-C or kill
			waitForTerminationRequest();
			// Stop the HTTPServer
			server.stop();
		}
		return Application::EXIT_OK;
	}
	
	bool									_isInteractive;
	bool									_helpRequested;
	SocketAddress*							_pCirrus;
	bool									_middle;
	Auth									_auth;
	string									_logDir;
	File*									_pLogFile;
	FileOutputStream						_logStream;
	FastMutex								_logMutex;
};

int main(int argc, char* argv[]) {
	DetectMemoryLeak();
	return CumulusService().run(argc, argv);
}
