#include <TestSupport.h>
#include <agents/HelperAgent/RequestHandler.h>
#include <agents/HelperAgent/RequestHandler.cpp>
#include <agents/HelperAgent/AgentOptions.h>
#include <ApplicationPool2/Pool.h>
#include <Utils/IOUtils.h>
#include <Utils/Timer.h>

#include <boost/shared_array.hpp>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <cstdarg>
#include <sys/socket.h>

using namespace std;
using namespace boost;
using namespace Passenger;
using namespace Passenger::ApplicationPool2;

namespace tut {
	struct RequestHandlerTest {
		ServerInstanceDirPtr serverInstanceDir;
		ServerInstanceDir::GenerationPtr generation;
		string serverFilename;
		FileDescriptor requestSocket;
		AgentOptions agentOptions;

		BackgroundEventLoop bg;
		SpawnerFactoryPtr spawnerFactory;
		PoolPtr pool;
		shared_ptr<RequestHandler> handler;
		FileDescriptor connection;
		map<string, string> defaultHeaders;

		string root;
		string rackAppPath, wsgiAppPath;
		
		RequestHandlerTest() {
			createServerInstanceDirAndGeneration(serverInstanceDir, generation);
			spawnerFactory = make_shared<SpawnerFactory>(bg.safe, *resourceLocator, generation);
			pool = make_shared<Pool>(bg.safe.get(), spawnerFactory);
			serverFilename = generation->getPath() + "/server";
			requestSocket = createUnixServer(serverFilename);
			setNonBlocking(requestSocket);

			agentOptions.passengerRoot = resourceLocator->getRoot();
			root = resourceLocator->getRoot();
			rackAppPath = root + "/test/stub/rack";
			wsgiAppPath = root + "/test/stub/wsgi";
			defaultHeaders["PASSENGER_LOAD_SHELL_ENVVARS"] = "false";
			defaultHeaders["PASSENGER_APP_TYPE"] = "wsgi";
			defaultHeaders["PASSENGER_SPAWN_METHOD"] = "direct";
			defaultHeaders["REQUEST_METHOD"] = "GET";
		}
		
		~RequestHandlerTest() {
			setLogLevel(0);
			unlink(serverFilename.c_str());
			handler.reset();
			pool->destroy();
			pool.reset();
		}

		void init() {
			handler = make_shared<RequestHandler>(bg.safe, requestSocket, pool, agentOptions);
			bg.start();
		}

		FileDescriptor &connect() {
			connection = connectToUnixServer(serverFilename);
			return connection;
		}

		void sendHeaders(const map<string, string> &headers, ...) {
			va_list ap;
			const char *arg;
			map<string, string>::const_iterator it;
			vector<StaticString> args;

			for (it = headers.begin(); it != headers.end(); it++) {
				args.push_back(StaticString(it->first.data(), it->first.size() + 1));
				args.push_back(StaticString(it->second.data(), it->second.size() + 1));
			}

			va_start(ap, headers);
			while ((arg = va_arg(ap, const char *)) != NULL) {
				args.push_back(StaticString(arg, strlen(arg) + 1));
			}
			va_end(ap);

			shared_array<StaticString> args_array(new StaticString[args.size() + 2]);
			unsigned int totalSize = 0;
			for (unsigned int i = 0; i < args.size(); i++) {
				args_array[i + 1] = args[i];
				totalSize += args[i].size();
			}
			char totalSizeString[10];
			snprintf(totalSizeString, sizeof(totalSizeString), "%u:", totalSize);
			args_array[0] = StaticString(totalSizeString);
			args_array[args.size() + 1] = ",";
			
			gatheredWrite(connection, args_array.get(), args.size() + 2, NULL);
		}

		string stripHeaders(const string &str) {
			string::size_type pos = str.find("\r\n\r\n");
			if (pos == string::npos) {
				return str;
			} else {
				string result = str;
				result.erase(0, pos + 4);
				return result;
			}
		}

		string inspect() {
			string result;
			bg.safe->runSync(boost::bind(&RequestHandlerTest::real_inspect, this, &result));
			return result;
		}

		void real_inspect(string *result) {
			stringstream stream;
			handler->inspect(stream);
			*result = stream.str();
		}
	};

	DEFINE_TEST_GROUP(RequestHandlerTest);

	TEST_METHOD(1) {
		// Test one normal request.
		init();
		connect();
		sendHeaders(defaultHeaders,
			"PASSENGER_APP_ROOT", wsgiAppPath.c_str(),
			"PATH_INFO", "/",
			NULL);
		string response = readAll(connection);
		string body = stripHeaders(response);
		ensure("Status line is correct", containsSubstring(response, "HTTP/1.1 200 OK\r\n"));
		ensure("Headers are correct", containsSubstring(response, "Content-Type: text/html\r\n"));
		ensure("Contains a Status header", containsSubstring(response, "Status: 200 OK\r\n"));
		ensure_equals(body, "hello <b>world</b>");
	}

	TEST_METHOD(2) {
		// Test multiple normal requests.
		init();
		for (int i = 0; i < 10; i++) {
			connect();
			sendHeaders(defaultHeaders,
				"PASSENGER_APP_ROOT", wsgiAppPath.c_str(),
				"PATH_INFO", "/",
				NULL);
			string response = readAll(connection);
			string body = stripHeaders(response);
			ensure("Status line is correct", containsSubstring(response, "HTTP/1.1 200 OK\r\n"));
			ensure("Headers are correct", containsSubstring(response, "Content-Type: text/html\r\n"));
			ensure("Contains a Status header", containsSubstring(response, "Status: 200 OK\r\n"));
			ensure_equals(body, "hello <b>world</b>");
		}
	}

	TEST_METHOD(3) {
		// Test sending request data in pieces.
		defaultHeaders["PASSENGER_APP_ROOT"] = wsgiAppPath;
		defaultHeaders["PATH_INFO"] = "/";

		string request;
		map<string, string>::const_iterator it, end = defaultHeaders.end();
		for (it = defaultHeaders.begin(); it != end; it++) {
			request.append(it->first);
			request.append(1, '\0');
			request.append(it->second);
			request.append(1, '\0');
		}
		request = toString(request.size()) + ":" + request;
		request.append(",");

		init();
		connect();
		string::size_type i = 0;
		while (i < request.size()) {
			const string piece = const_cast<const string &>(request).substr(i, 5);
			writeExact(connection, piece);
			usleep(10000);
			i += piece.size();
		}

		string response = readAll(connection);
		string body = stripHeaders(response);
		ensure("Status line is correct", containsSubstring(response, "HTTP/1.1 200 OK\r\n"));
		ensure("Headers are correct", containsSubstring(response, "Content-Type: text/html\r\n"));
		ensure("Contains a Status header", containsSubstring(response, "Status: 200 OK\r\n"));
		ensure_equals(body, "hello <b>world</b>");
	}

	TEST_METHOD(4) {
		// It denies access if the connect password is wrong.
		agentOptions.requestSocketPassword = "hello world";
		setLogLevel(-1);
		init();

		connect();
		writeExact(connection, "hello world");
		sendHeaders(defaultHeaders,
			"PASSENGER_APP_ROOT", wsgiAppPath.c_str(),
			"PATH_INFO", "/",
			NULL
		);
		ensure(containsSubstring(readAll(connection), "hello <b>world</b>"));

		connect();
		try {
			sendHeaders(defaultHeaders,
				"PASSENGER_APP_ROOT", wsgiAppPath.c_str(),
				"PATH_INFO", "/",
				NULL
			);
		} catch (const SystemException &e) {
			ensure_equals(e.code(), EPIPE);
			return;
		}
		string response;
		try {
			response = readAll(connection);
		} catch (const SystemException &e) {
			ensure_equals(e.code(), ECONNRESET);
			return;
		}
		ensure_equals(response, "");
	}

	TEST_METHOD(5) {
		// It disconnects us if the connect password is not sent within a certain time.
		agentOptions.requestSocketPassword = "hello world";
		setLogLevel(-1);
		handler = make_shared<RequestHandler>(bg.safe, requestSocket, pool, agentOptions);
		handler->connectPasswordTimeout = 40;
		bg.start();

		connect();
		Timer timer;
		readAll(connection);
		timer.stop();
		ensure(timer.elapsed() <= 60);
	}

	TEST_METHOD(6) {
		// It works correct if the connect password is sent in pieces.
		agentOptions.requestSocketPassword = "hello world";
		init();
		connect();
		writeExact(connection, "hello");
		usleep(10000);
		writeExact(connection, " world");
		usleep(10000);
		sendHeaders(defaultHeaders,
			"PASSENGER_APP_ROOT", wsgiAppPath.c_str(),
			"PATH_INFO", "/",
			NULL
		);
		ensure(containsSubstring(readAll(connection), "hello <b>world</b>"));
	}

	TEST_METHOD(7) {
		// It closes the connection with the application if the client has closed the connection.
		init();
		connect();
		sendHeaders(defaultHeaders,
			"PASSENGER_APP_ROOT", wsgiAppPath.c_str(),
			"PATH_INFO", "/stream",
			NULL
		);
		BufferedIO io(connection);
		ensure_equals(io.readLine(), "HTTP/1.1 200 OK\r\n");
		ensure_equals(pool->getProcessCount(), 1u);
		SuperGroupPtr superGroup = pool->superGroups.get(wsgiAppPath);
		ProcessPtr process = superGroup->defaultGroup->processes.front();
		ensure_equals(process->sessions, 1);
		connection.close();
		EVENTUALLY(5,
			result = process->sessions == 0;
		);
	}
	
	TEST_METHOD(10) {
		// If the app crashes at startup without an error page then it renders
		// a generic error page.
		TempDir tempdir("tmp.handler");
		writeFile("tmp.handler/start.rb",
			"STDERR.puts 'I have failed'");

		setLogLevel(-2);
		spawnerFactory->forwardStderr = false;
		init();
		connect();
		sendHeaders(defaultHeaders,
			"PASSENGER_APP_ROOT", (root + "/test/tmp.handler").c_str(),
			"PASSENGER_APP_TYPE", "",
			"PASSENGER_START_COMMAND", ("ruby\1" + root + "/test/tmp.handler/start.rb").c_str(),
			"PATH_INFO", "/",
			NULL);
		string response = readAll(connection);
		ensure(containsSubstring(response, "HTTP/1.1 500 Internal Server Error\r\n"));
		ensure(containsSubstring(response, "Status: 500 Internal Server Error\r\n"));
		ensure(containsSubstring(response, "I have failed"));
	}

	TEST_METHOD(11) {
		// If the app crashes at startup with an error page then it renders
		// a friendly error page.
		TempDir tempdir("tmp.handler");
		writeFile("tmp.handler/start.rb",
			"STDERR.puts 'Error'\n"
			"STDERR.puts\n"
			"STDERR.puts 'I have failed'\n");

		setLogLevel(-2);
		spawnerFactory->forwardStderr = false;
		init();
		connect();
		sendHeaders(defaultHeaders,
			"PASSENGER_APP_ROOT", (root + "/test/tmp.handler").c_str(),
			"PASSENGER_APP_TYPE", "",
			"PASSENGER_START_COMMAND", ("ruby\1" + root + "/test/tmp.handler/start.rb").c_str(),
			"PATH_INFO", "/",
			NULL);
		string response = readAll(connection);
		ensure(containsSubstring(response, "HTTP/1.1 500 Internal Server Error\r\n"));
		ensure(containsSubstring(response, "Status: 500 Internal Server Error\r\n"));
		ensure(containsSubstring(response, "Content-Type: text/html; charset=UTF-8\r\n"));
		ensure(containsSubstring(response, "<html>"));
		ensure(containsSubstring(response, "I have failed"));
	}

	TEST_METHOD(12) {
		// If spawning fails because of an internal error then it reports the error appropriately.
		TempDir tempdir("tmp.handler");
		writeFile("tmp.handler/start.rb", "");

		setLogLevel(-2);
		spawnerFactory->forwardStderr = false;
		init();
		connect();
		sendHeaders(defaultHeaders,
			"PASSENGER_APP_ROOT", (root + "/test/tmp.handler").c_str(),
			"PASSENGER_APP_TYPE", "",
			"PASSENGER_START_COMMAND", ("ruby\1" + root + "/test/tmp.handler/start.rb").c_str(),
			"PASSENGER_RAISE_INTERNAL_ERROR", "true",
			"PATH_INFO", "/",
			NULL);
		string response = readAll(connection);
		ensure(containsSubstring(response, "HTTP/1.1 500 Internal Server Error\r\n"));
		ensure(containsSubstring(response, "Status: 500 Internal Server Error\r\n"));
		ensure(containsSubstring(response, "Content-Type: text/html; charset=UTF-8\r\n"));
		ensure(containsSubstring(response, "<html>"));
		ensure(containsSubstring(response, "An internal error occurred while trying to spawn the application."));
		ensure(containsSubstring(response, "Passenger:<wbr>:<wbr>RuntimeException"));
		ensure(containsSubstring(response, "An internal error!"));
		ensure(containsSubstring(response, "Spawner.h"));
	}

	TEST_METHOD(13) {
		// Error pages respect the PASSENGER_STATUS_LINE option.
		TempDir tempdir("tmp.handler");
		writeFile("tmp.handler/start.rb",
			"STDERR.puts 'I have failed'");

		setLogLevel(-2);
		spawnerFactory->forwardStderr = false;
		init();
		connect();
		sendHeaders(defaultHeaders,
			"PASSENGER_APP_ROOT", (root + "/test/tmp.handler").c_str(),
			"PASSENGER_APP_TYPE", "",
			"PASSENGER_START_COMMAND", ("ruby\1" + root + "/test/tmp.handler/start.rb").c_str(),
			"PASSENGER_STATUS_LINE", "false",
			"PATH_INFO", "/",
			NULL);
		string response = readAll(connection);
		ensure(!containsSubstring(response, "HTTP/1.1 "));
		ensure(containsSubstring(response, "Status: 500 Internal Server Error\r\n"));
		ensure(containsSubstring(response, "I have failed"));
	}

	TEST_METHOD(14) {
		// If PASSENGER_FRIENDLY_ERROR_PAGES is false then it does not render
		// a friendly error page.
		TempDir tempdir("tmp.handler");
		writeFile("tmp.handler/start.rb",
			"STDERR.puts 'Error'\n"
			"STDERR.puts\n"
			"STDERR.puts 'I have failed'\n");

		setLogLevel(-2);
		spawnerFactory->forwardStderr = false;
		init();
		connect();
		sendHeaders(defaultHeaders,
			"PASSENGER_APP_ROOT", (root + "/test/tmp.handler").c_str(),
			"PASSENGER_APP_TYPE", "",
			"PASSENGER_START_COMMAND", ("ruby\1" + root + "/test/tmp.handler/start.rb").c_str(),
			"PASSENGER_FRIENDLY_ERROR_PAGES", "false",
			"PATH_INFO", "/",
			NULL);
		string response = readAll(connection);
		ensure(containsSubstring(response, "HTTP/1.1 500 Internal Server Error\r\n"));
		ensure(containsSubstring(response, "Status: 500 Internal Server Error\r\n"));
		ensure(containsSubstring(response, "Content-Type: text/html; charset=UTF-8\r\n"));
		ensure(containsSubstring(response, "<html>"));
		ensure(!containsSubstring(response, "I have failed"));
		ensure(containsSubstring(response, "We're sorry, but something went wrong"));
	}

	TEST_METHOD(20) {
		// It streams the request body to the application.
		DeleteFileEventually file("tmp.output");

		init();
		connect();
		sendHeaders(defaultHeaders,
			"PASSENGER_APP_ROOT", wsgiAppPath.c_str(),
			"PATH_INFO", "/upload",
			"HTTP_X_OUTPUT", (root + "/test/tmp.output").c_str(),
			NULL);
		writeExact(connection, "hello\n");
		EVENTUALLY(5,
			result = fileExists("tmp.output") && readAll("tmp.output") == "hello\n";
		);
		writeExact(connection, "world\n");
		EVENTUALLY(3,
			result = readAll("tmp.output") == "hello\nworld\n";
		);
		shutdown(connection, SHUT_WR);
		ensure_equals(stripHeaders(readAll(connection)), "ok");
	}

	TEST_METHOD(21) {
		// It buffers the request body if PASSENGER_BUFFERING is true.
		DeleteFileEventually file("tmp.output");

		init();
		connect();
		sendHeaders(defaultHeaders,
			"PASSENGER_APP_ROOT", wsgiAppPath.c_str(),
			"PASSENGER_BUFFERING", "true",
			"PATH_INFO", "/upload",
			"HTTP_X_OUTPUT", (root + "/test/tmp.output").c_str(),
			NULL);
		writeExact(connection, "hello\n");
		SHOULD_NEVER_HAPPEN(200,
			result = fileExists("tmp.output");
		);
		writeExact(connection, "world\n");
		SHOULD_NEVER_HAPPEN(200,
			result = fileExists("tmp.output");
		);
		shutdown(connection, SHUT_WR);
		ensure_equals(stripHeaders(readAll(connection)), "ok");
	}

	TEST_METHOD(22) {
		set_test_name("Test buffering of large request bodies that fit in neither the socket "
		              "buffer nor the FileBackedPipe memory buffer, and that the application "
		              "cannot read quickly enough.");

		DeleteFileEventually d1("/tmp/wait.txt");
		DeleteFileEventually d2("/tmp/output.txt");

		// 2.6 MB of request body. Guaranteed not to fit in any socket buffer.
		string requestBody;
		for (int i = 0; i < 204800; i++) {
			requestBody.append("hello world!\n");
		}

		init();
		connect();
		sendHeaders(defaultHeaders,
			"PASSENGER_APP_ROOT", wsgiAppPath.c_str(),
			"PATH_INFO", "/upload",
			"PASSENGER_BUFFERING", "true",
			"HTTP_X_WAIT_FOR_FILE", "/tmp/wait.txt",
			"HTTP_X_OUTPUT", "/tmp/output.txt",
			NULL);
		
		// Should not block.
		writeExact(connection, requestBody);
		shutdown(connection, SHUT_WR);
		
		EVENTUALLY(5,
			result = containsSubstring(inspect(), "session initiated           = true");
		);
		touchFile("/tmp/wait.txt");

		string result = stripHeaders(readAll(connection));
		ensure_equals(result, "ok");
		struct stat buf;
		ensure(stat("/tmp/output.txt", &buf) == 0);
		ensure_equals(buf.st_size, (off_t) requestBody.size());
	}

	TEST_METHOD(30) {
		// It replaces HTTP_CONTENT_LENGTH with CONTENT_LENGTH.
		init();
		connect();
		sendHeaders(defaultHeaders,
			"PASSENGER_APP_ROOT", wsgiAppPath.c_str(),
			"PATH_INFO", "/env",
			"HTTP_CONTENT_LENGTH", "5",
			NULL);
		writeExact(connection, "hello");
		string response = readAll(connection);
		ensure(containsSubstring(response, "CONTENT_LENGTH = 5\n"));
		ensure(!containsSubstring(response, "HTTP_CONTENT_LENGTH"));
	}
	
	TEST_METHOD(31) {
		// It replaces HTTP_CONTENT_TYPE with CONTENT_TYPE.
		init();
		connect();
		sendHeaders(defaultHeaders,
			"PASSENGER_APP_ROOT", wsgiAppPath.c_str(),
			"PATH_INFO", "/env",
			"HTTP_CONTENT_TYPE", "application/json",
			NULL);
		string response = readAll(connection);
		ensure(containsSubstring(response, "CONTENT_TYPE = application/json\n"));
		ensure(!containsSubstring(response, "HTTP_CONTENT_TYPE"));
	}

	TEST_METHOD(35) {
		// The response doesn't contain an HTTP status line if PASSENGER_STATUS_LINE is false.
		init();
		connect();
		sendHeaders(defaultHeaders,
			"PASSENGER_APP_ROOT", wsgiAppPath.c_str(),
			"PASSENGER_STATUS_LINE", "false",
			"PATH_INFO", "/",
			NULL);
		string response = readAll(connection);
		ensure(!containsSubstring(response, "HTTP/1.1 "));
		ensure(containsSubstring(response, "Status: 200 OK\r\n"));
	}

	TEST_METHOD(36) {
		// If the application outputs a status line without a reason phrase,
		// then a reason phrase is automatically appended.
		init();
		connect();
		sendHeaders(defaultHeaders,
			"PASSENGER_APP_ROOT", wsgiAppPath.c_str(),
			"PATH_INFO", "/custom_status",
			"HTTP_X_CUSTOM_STATUS", "201",
			NULL);
		string response = readAll(connection);
		ensure(containsSubstring(response, "HTTP/1.1 201 Created\r\n"));
		ensure(containsSubstring(response, "Status: 201 Created\r\n"));
	}

	TEST_METHOD(37) {
		// If the application outputs a status line with a custom reason phrase,
		// then that reason phrase is used.
		init();
		connect();
		sendHeaders(defaultHeaders,
			"PASSENGER_APP_ROOT", wsgiAppPath.c_str(),
			"PATH_INFO", "/custom_status",
			"HTTP_X_CUSTOM_STATUS", "201 Bunnies Jump",
			NULL);
		string response = readAll(connection);
		ensure(containsSubstring(response, "HTTP/1.1 201 Bunnies Jump\r\n"));
		ensure(containsSubstring(response, "Status: 201 Bunnies Jump\r\n"));
	}
	
	TEST_METHOD(38) {
		// If the application doesn't output a status line then it rejects the application response.
		// TODO
	}

	TEST_METHOD(39) {
		// Test handling of slow clients that can't receive response data fast enough (response buffering).
		init();
		connect();
		sendHeaders(defaultHeaders,
			"PASSENGER_APP_ROOT", wsgiAppPath.c_str(),
			"PATH_INFO", "/blob",
			"HTTP_X_SIZE", "10485760",
			NULL);
		EVENTUALLY(10,
			result = containsSubstring(inspect(), "appInput reachedEnd         = true");
		);
		string result = stripHeaders(readAll(connection));
		ensure_equals(result.size(), 10485760u);
		const char *data = result.data();
		const char *end  = result.data() + result.size();
		while (data < end) {
			ensure_equals(*data, 'x');
			data++;
		}
	}

	// Test application that reads the client body slower than the client sends it.
	// Test that RequestHandler does not pass any client body data when CONTENT_LENGTH == 0 (when buffering is on).
	// Test that RequestHandler does not pass any client body data when CONTENT_LENGTH == 0 (when buffering is off).
	// Test that RequestHandler does not read more than CONTENT_LENGTH bytes from the client body (when buffering is on).
	// Test that RequestHandler does not read more than CONTENT_LENGTH bytes from the client body (when buffering is off).
	// Test small response buffering.
	// Test large response buffering.
}
