#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <ShellScalingApi.h>
#define inline __inline

#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "obs-cli.h"
#include <jansson.h>
#include "util/threading.h"
#include "obs-scene.h"

#ifndef _WIN32
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/errno.h>
static int sockfd = -1;
static bool socket_ready = false;
#else
#define ssize_t long
static SOCKET sock = INVALID_SOCKET;
#endif

// The three primary sources: audio, desktop, and webcam
static obs_source_t *audioSource = NULL;
static obs_source_t *displaySource = NULL;
static obs_source_t *webcamSource = NULL;

// Output file for recordings
static obs_output_t *fileOutput = NULL;

static obs_encoder_t *encoder = NULL;
static obs_encoder_t *audioEncoder = NULL;
static obs_scene_t *scene = NULL;

static int s_output_width = 0;
static int s_output_height = 0;
static int s_output_slice_x = 0;
static int s_output_slice_y = 0;
static int s_output_slice_width = 0;
static int s_output_slice_height = 0;
static uint8_t* s_output_slice_buffer = NULL;
static bool s_raw_output_active = false;

// Array of scenes
static obs_scene_t **sceneList = NULL;
static int numScenes = 0;

// We write to stdout when we get certain events from
// obs and callbacks are not threadsafe, so we coordinate writes.
static pthread_mutex_t stdout_mutex;

static void null_log_handler(int log_level, const char *format, va_list args,
			     void *param)
{
}

static void connect_to_local(int port)
	{
#ifndef _WIN32
	socket_ready = false;
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		fprintf(stderr, "ERROR opening socket: %d\n", errno);
		return;
	}

	struct sockaddr_in serv_addr;
	bzero((char *)&serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(port);
	serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	fprintf(stderr, "Connecting to localhost %d ...\n", port);

	if (connect(sockfd, (struct sockaddr *)&serv_addr,
		    sizeof(struct sockaddr_in)) < 0) {
		fprintf(stderr, "ERROR connecting to port %d: %d\n", port,
			errno);
		close(sockfd);
		sockfd = -1;
	}

	fprintf(stderr, "Connected!\n");

	socket_ready = true;

#else
	printf("Connecting to %d\n", port);

	int iResult = 0;

	WSADATA wsaData = {0};
	iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		printf(L"WSAStartup failed: %d\n", iResult);
		return 1;
	}

	// If there was an existing socket - close it
	if (sock != INVALID_SOCKET) {
		closesocket(sock);
	}

	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	struct sockaddr_in clientService;
	clientService.sin_family = AF_INET;
	clientService.sin_addr.s_addr = inet_addr("127.0.0.1");
	clientService.sin_port = htons(port);

	//----------------------
	// Connect to server.
	iResult = connect(sock, (SOCKADDR *)&clientService,
			  sizeof(clientService));
	if (iResult == SOCKET_ERROR) {
		wprintf(L"connect function failed with error: %ld\n",
			WSAGetLastError());
		iResult = closesocket(sock);
		if (iResult == SOCKET_ERROR)
			wprintf(L"closesocket function failed with error: %ld\n",
				WSAGetLastError());
		WSACleanup();
		return 1;
	}

	printf("Started socket for render frames\n");
#endif
}

static void disconnect_from_local()
{
#ifndef _WIN32
	if (sockfd != -1) {
		fprintf(stderr, "Closing socket\n");
		close(sockfd);
		sockfd = -1;
	}
#else
	// If there was an existing socket - close it
	if (sock != INVALID_SOCKET) {
		closesocket(sock);
		sock = INVALID_SOCKET;
	}
#endif
}

static void receive_video(void *param, struct video_data *frame)
{
	int frame_size = s_output_width * s_output_height * 4;
	if (frame_size == 0) {
		return;
	}

	// If we are only sending back a slice, copy slice into buffer
	if (s_output_slice_buffer != NULL) {
		for (int i=0;i < s_output_slice_height; i++) {
			int sourceOffset = ((s_output_slice_y + i) * s_output_width + s_output_slice_x) * 4;
			int destOffset = (i * s_output_slice_width) * 4;
			int numBytesToCopy = s_output_slice_width * 4;
			memcpy(s_output_slice_buffer + destOffset, frame->data[0] + sourceOffset, numBytesToCopy);
		}
	}

	int bytesToWrite = frame_size;
	if (s_output_slice_buffer) {
		bytesToWrite = s_output_slice_height * s_output_slice_width * 4;
	}

#ifndef _WIN32
	if (sockfd == -1 || !socket_ready) {
		return;
	}

	ssize_t bytesWritten = 0;

	if (s_output_slice_buffer != NULL) {
		bytesWritten = write(sockfd, s_output_slice_buffer, bytesToWrite);
	} else {
		bytesWritten = write(sockfd, frame->data[0], bytesToWrite);
	}

	if (bytesWritten != frame_size) {
		disconnect_from_local;
	}
#else
	if (sock == INVALID_SOCKET) {
		return;
	}

	int bytesWritten = 0;

	if (s_output_slice_buffer != NULL) {
		bytesWritten = send(sock, s_output_slice_buffer, bytesToWrite, 0);
	} else {
		bytesWritten = send(sock, frame->data[0], bytesToWrite, 0);
	}

	if (bytesWritten != bytesToWrite) {
		disconnect_from_local();
	}
#endif
}

static void stop_raw_output()
{
	if (s_raw_output_active)
	{
		obs_remove_raw_video_callback(receive_video, NULL);
		s_raw_output_active = false;
	}
}

static void start_raw_output()
{
	if (!s_raw_output_active)
	{
		struct video_scale_info info = {0};
		info.format = VIDEO_FORMAT_RGBA;
		info.width = s_output_width;
		info.height = s_output_height;
		obs_add_raw_video_callback(&info, receive_video, NULL);
		s_raw_output_active = true;
	}
}

static void output_stopped(void *my_data, calldata_t *cd)
{
	obs_output_t *output = calldata_ptr(cd, "output");
	char *actionId = (char *)my_data;

	pthread_mutex_lock(&stdout_mutex);
	fprintf(stderr, "STOPPED!!!!");
	fprintf(stdout, "\n{ \"actionId\": \"%s\"}\n", actionId);
	fflush(stdout);
	pthread_mutex_unlock(&stdout_mutex);

	bfree(my_data);
}

static int initialize(json_t *obj)
{
	if (pthread_mutex_init(&stdout_mutex, NULL) != 0) {
		fprintf(stderr, "error initializing stdout mutex");
		return 0;
	}

	blog(LOG_INFO, "Starting OBS!");

	json_t *pluginDirObj = json_object_get(obj, "pluginDir");
	if (!json_is_string(pluginDirObj)) {
		fprintf(stderr, "error: pluginDir is not a string\n");
		return 0;
	}
	const char *pluginDir = json_string_value(pluginDirObj);

	json_t *exeDirObj = json_object_get(obj, "exeDir");
	if (!json_is_string(exeDirObj)) {
		fprintf(stderr, "error: exeDir is not a string\n");
		return 0;
	}
	const char *exeDir = json_string_value(exeDirObj);
	obs_set_executable_path(exeDir);

	json_t *dataDirObj = json_object_get(obj, "dataDir");
	if (!json_is_string(dataDirObj)) {
		fprintf(stderr, "error: dataDir is not a string\n");
		return 0;
	}
	const char *dataDir = json_string_value(dataDirObj);
	obs_add_data_path(dataDir);

	if (!obs_startup("en", ".", NULL)) {
		blog(LOG_ERROR, "Failed to start");
		return 1;
	} else {
		blog(LOG_INFO, "Started OBS");
	}

	struct obs_video_info ovi;
	ovi.adapter = 0;
	ovi.gpu_conversion = false;
#ifndef _WIN32
	ovi.graphics_module = "libobs-opengl";
#else
	ovi.graphics_module = "libobs-d3d11";
#endif
	ovi.fps_num = 30000;
	ovi.fps_den = 1000;
	ovi.base_width = 1280;
	ovi.base_height = 720;
	ovi.output_width = 1280;
	ovi.output_height = 720;
	ovi.output_format = VIDEO_FORMAT_RGBA;
	ovi.scale_type = OBS_SCALE_BILINEAR;

	blog(LOG_INFO, "Resetting video");
	int rc = obs_reset_video(&ovi);
	blog(LOG_INFO, "Result: %d", rc);

	struct obs_audio_info ai;
	ai.samples_per_sec = 44100;
	ai.speakers = SPEAKERS_MONO;
	rc = obs_reset_audio(&ai);
	blog(LOG_INFO, "Reset audio: %d", rc);

	obs_add_module_path(pluginDir, pluginDir);
	blog(LOG_INFO, "Loading modules");
	obs_load_all_modules();
	obs_post_load_modules();
	blog(LOG_INFO, "Done loading modules");

#ifndef _WIN32
	displaySource = obs_source_create("display_capture", "Display Capture",
					  NULL, NULL);
	if (!displaySource) {
		blog(LOG_ERROR, "Unable to create source");
		return 1;
	} else {
		blog(LOG_INFO, "Created display capture!");
	}

	audioSource = obs_source_create("coreaudio_input_capture", "Microphone",
					NULL, NULL);
	if (!audioSource) {
		blog(LOG_ERROR, "Unable to create audio source");
	} else {
		blog(LOG_INFO, "Created audio source");
	}

	webcamSource = obs_source_create("av_capture_input", "Webcam Capture",
					 NULL, NULL);
	if (!webcamSource) {
		blog(LOG_ERROR, "Unable to create webcam source");
	} else {
		blog(LOG_INFO, "created av capture!");
	}
#else
	displaySource = obs_source_create("monitor_capture", "Display Capture",
					  NULL, NULL);
	if (!displaySource) {
		blog(LOG_ERROR, "Unable to create source");
		return 1;
	} else {
		blog(LOG_INFO, "Created display capture!");
	}

	audioSource = obs_source_create("wasapi_input_capture", "Microphone",
					NULL, NULL);
	if (!audioSource) {
		blog(LOG_ERROR, "Unable to create audio source");
	} else {
		blog(LOG_INFO, "Created audio source");
	}

	webcamSource =
		obs_source_create("dshow_input", "Webcam Capture", NULL, NULL);
	if (!webcamSource) {
		blog(LOG_ERROR, "Unable to create webcam source");
	} else {
		blog(LOG_INFO, "created av capture!");
	}
#endif

	return 0;
}

// Set up video for recording a single source to the output
static int initializeSingleVideoRecording(json_t *obj)
{
	blog(LOG_INFO, "In initializeSingleVideoRecording");

	json_t *inputWidthObj = json_object_get(obj, "inputWidth");
	if (!json_is_integer(inputWidthObj)) {
		fprintf(stderr, "error: inputWidth is not an integer\n");
		return 1;
	}
	int inputWidth = json_integer_value(inputWidthObj);

	json_t *inputHeightObj = json_object_get(obj, "inputHeight");
	if (!json_is_integer(inputHeightObj)) {
		fprintf(stderr, "error: inputHeight is not an integer\n");
		return 1;
	}
	int inputHeight = json_integer_value(inputHeightObj);

	json_t *outputWidthObj = json_object_get(obj, "outputWidth");
	if (!json_is_integer(outputWidthObj)) {
		fprintf(stderr, "error: outputWidth is not an integer\n");
		return 1;
	}
	int outputWidth = json_integer_value(outputWidthObj);

	json_t *outputHeightObj = json_object_get(obj, "outputHeight");
	if (!json_is_integer(outputHeightObj)) {
		fprintf(stderr, "error: outputHeight is not an integer\n");
		return 1;
	}
	int outputHeight = json_integer_value(outputHeightObj);

	json_t *scaledWidthObj = json_object_get(obj, "scaledWidth");
	if (!json_is_integer(scaledWidthObj)) {
		fprintf(stderr, "error: scaledWidth is not an integer\n");
		return 1;
	}
	s_output_width = json_integer_value(scaledWidthObj);

	json_t *scaledHeightObj = json_object_get(obj, "scaledHeight");
	if (!json_is_integer(scaledHeightObj)) {
		fprintf(stderr, "error: scaledHeight is not an integer\n");
		return 1;
	}
	s_output_height = json_integer_value(scaledHeightObj);

	json_t *deviceTypeObj = json_object_get(obj, "deviceType");
	if (!json_is_string(deviceTypeObj)) {
		fprintf(stderr, "error: deviceTypeObj is not a string\n");
		return 1;
	}
	const char *deviceType = json_string_value(deviceTypeObj);

	int cropLeft = 0;
	int cropRight = 0;
	int cropTop = 0;
	int cropBottom = 0;

	json_t *cropLeftObj = json_object_get(obj, "cropLeft");
	if (cropLeftObj) {
		cropLeft = json_integer_value(cropLeftObj);
	}
	json_t *cropRightObj = json_object_get(obj, "cropRight");
	if (cropRightObj) {
		cropRight = json_integer_value(cropRightObj);
	}
	json_t *cropTopObj = json_object_get(obj, "cropTop");
	if (cropTopObj) {
		cropTop = json_integer_value(cropTopObj);
	}
	json_t *cropBottomObj = json_object_get(obj, "cropBottom");
	if (cropBottomObj) {
		cropBottom = json_integer_value(cropBottomObj);
	}

	blog(LOG_INFO, "Device type: %s", deviceType);

	if (strcmp(deviceType, "monitor") == 0) {
		blog(LOG_INFO, "initializing monitor");
		json_t *displayNumObj = json_object_get(obj, "displayNum");
		if (!json_is_integer(displayNumObj)) {
			fprintf(stderr,
				"error: displayNum is not an integer\n");
			return 1;
		}
		int displayNum = json_integer_value(displayNumObj);

		obs_data_t *displaySettings = obs_data_create();

#ifndef _WIN32
		obs_data_set_int(displaySettings, "display", displayNum);
#else
		obs_data_set_int(displaySettings, "monitor", displayNum);
#endif

		obs_source_update(displaySource, displaySettings);

		// Delete old scene
		if (scene) {
			obs_set_output_source(0, NULL);
			obs_scene_release(scene);
		}

		scene = obs_scene_create("Test");
		obs_scene_addref(scene);

		struct obs_scene_item *item;

		item = obs_scene_add(scene, displaySource);
		if (item == NULL) {
			blog(LOG_ERROR, "Could not add scene item");
		} else {
			blog(LOG_INFO, "Added video to scene");
		}
		item->crop.left = cropLeft;
		item->crop.top = cropTop;
		item->crop.right = cropRight;
		item->crop.bottom = cropBottom;

		obs_sceneitem_force_update_transform(item);

		item = obs_scene_add(scene, audioSource);
		if (item == NULL) {
			blog(LOG_ERROR, "Could not add scene item");
		} else {
			blog(LOG_INFO, "Added audio to scene");
		}

		// Note you must get the source from the scene
		obs_set_output_source(0, obs_scene_get_source(scene));

		blog(LOG_INFO, "Set display to %d", displayNum);
	} else if (strcmp(deviceType, "webcam") == 0) {
		blog(LOG_INFO, "initializing webcam");

		json_t *deviceIdObj = json_object_get(obj, "deviceId");
		if (!json_is_string(deviceIdObj)) {
			fprintf(stderr, "error: deviceIdObj is not a string\n");
			return 0;
		}

		const char *deviceId = json_string_value(deviceIdObj);
		obs_data_t *settings = obs_data_create();

#ifndef _WIN32
		// osx wants json obj for resolution
		char resolution[128];
		sprintf(resolution, "{ \"width\": %d, \"height\": %d }",
			inputWidth, inputHeight);

		json_t *numeratorObj = json_object_get(obj, "fpsNumerator");
		if (!json_is_number(numeratorObj)) {
			fprintf(stderr, "error: fpsNum is not a number\n");
			return 0;
		}
		int numerator = json_integer_value(numeratorObj);

		json_t *denominatorObj = json_object_get(obj, "fpsDenominator");
		if (!json_is_number(denominatorObj)) {
			fprintf(stderr,
				"error: denominatorObj is not a number\n");
			return 0;
		}
		int denominator = json_integer_value(denominatorObj);

		obs_data_set_string(settings, "device", deviceId);
		obs_data_set_bool(settings, "use_preset", false);
		obs_data_set_string(settings, "resolution", resolution);

		struct media_frames_per_second fps;
		fps.numerator = numerator;
		fps.denominator = denominator;
		obs_data_set_frames_per_second(settings, "frame_rate", fps,
					       NULL);
#else
		char resolution[32];
		sprintf(resolution, "%dx%d", inputWidth, inputHeight);

		obs_data_set_string(settings, "video_device_id", deviceId);
		obs_data_set_string(settings, "resolution", resolution);
		// Custom resolution
		obs_data_set_int(settings, "res_type", 1);
#endif

		obs_source_update(webcamSource, settings);

		obs_set_output_source(0, webcamSource);

		blog(LOG_INFO, "Set webcam to %s", deviceId);
	} else {
		blog(LOG_ERROR, "Unknown device type: %s", deviceType);
		return 1;
	}

	struct obs_video_info ovi;
	ovi.adapter = 0;
	ovi.gpu_conversion = false;
#ifndef _WIN32
	ovi.graphics_module = "libobs-opengl";
#else
	ovi.graphics_module = "libobs-d3d11";
#endif
	ovi.fps_num = 30000;
	ovi.fps_den = 1000;
	ovi.base_width = inputWidth;
	ovi.base_height = inputHeight;
	ovi.output_width = outputWidth;
	ovi.output_height = outputHeight;
	ovi.output_format = VIDEO_FORMAT_RGBA;
	ovi.scale_type = OBS_SCALE_BILINEAR;

	// Make sure to remove listener so that video is not considered active
	stop_raw_output();

	blog(LOG_INFO, "Resetting video");
	int rc = obs_reset_video(&ovi);
	blog(LOG_INFO, "Result: %d", rc);

	// Now add raw video callback
	start_raw_output();

	return 0;
}

static const int initializeDisplay(json_t *obj)
{
	blog(LOG_INFO, "initializing display");
	json_t *displayNumObj = json_object_get(obj, "displayNum");

	// Windows passes the handle to the capture window, which we will use
	// to get the monitor, and then the display index.
	json_t *captureWindowHandle = json_object_get(obj, "captureWindowHandle");

	if (!json_is_integer(displayNumObj) && !json_is_integer(captureWindowHandle)) {
		fprintf(stderr, "error: displayNum or captureWindowHandle must be passed in\n");
		return 1;
	}

	obs_data_t *displaySettings = obs_data_create();

#ifdef _WIN32
	int captureWindowHandleAsInt = json_integer_value(captureWindowHandle);

	HMONITOR monitor = MonitorFromWindow((HWND)captureWindowHandleAsInt, MONITOR_DEFAULTTONEAREST);

	// Get display devices
	json_t *deviceList = obs_source_device_list(displaySource);

	// Find matching monitor
	int monitorIndex = -1;
	int numMonitors = json_array_size(deviceList);
	for (int i = 0; i < numMonitors; i++) {
		json_t* monitorInfo = json_array_get(deviceList, i);
		json_t* monitorHandle = json_object_get(monitorInfo, "monitorHandle");
		if ((HMONITOR)json_integer_value(monitorHandle) == monitor) {
			blog(LOG_INFO, "Found monitor index: %d", i);
			monitorIndex = i;
			break;
		}
	}

	if (monitorIndex == -1) {
		fprintf(stderr,	"error: No monitor found for window handle %d\n",captureWindowHandleAsInt);
		return 1;
	}
	blog(LOG_INFO, "Set monitor to %d", monitorIndex);

	obs_data_set_int(displaySettings, "monitor", monitorIndex);
#else
	int displayNum = json_integer_value(displayNumObj);
	obs_data_set_int(displaySettings, "display", displayNum);
	blog(LOG_INFO, "Set display to %d", displayNum);
#endif

	obs_source_update(displaySource, displaySettings);

	return 0;
}

static const int initializeWebcam(json_t *obj)
{
	blog(LOG_INFO, "initializing webcam");

	json_t *inputWidthObj = json_object_get(obj, "inputWidth");
	if (!json_is_integer(inputWidthObj)) {
		fprintf(stderr, "error: inputWidth is not an integer\n");
		return 1;
	}
	int inputWidth = json_integer_value(inputWidthObj);

	json_t *inputHeightObj = json_object_get(obj, "inputHeight");
	if (!json_is_integer(inputHeightObj)) {
		fprintf(stderr, "error: inputHeight is not an integer\n");
		return 1;
	}
	int inputHeight = json_integer_value(inputHeightObj);

	json_t *deviceIdObj = json_object_get(obj, "deviceId");
	if (!json_is_string(deviceIdObj)) {
		fprintf(stderr, "error: deviceIdObj is not a string\n");
		return 0;
	}

	const char *deviceId = json_string_value(deviceIdObj);
	obs_data_t *settings = obs_data_create();

#ifndef _WIN32
	// osx wants json obj for resolution
	char resolution[128];
	sprintf(resolution, "{ \"width\": %d, \"height\": %d }", inputWidth,
		inputHeight);

	json_t *numeratorObj = json_object_get(obj, "fpsNumerator");
	if (!json_is_number(numeratorObj)) {
		fprintf(stderr, "error: fpsNum is not a number\n");
		return 0;
	}
	int numerator = json_integer_value(numeratorObj);

	json_t *denominatorObj = json_object_get(obj, "fpsDenominator");
	if (!json_is_number(denominatorObj)) {
		fprintf(stderr, "error: denominatorObj is not a number\n");
		return 0;
	}
	int denominator = json_integer_value(denominatorObj);

	obs_data_set_string(settings, "device", deviceId);
	obs_data_set_bool(settings, "use_preset", false);
	obs_data_set_string(settings, "resolution", resolution);

	struct media_frames_per_second fps;
	fps.numerator = numerator;
	fps.denominator = denominator;
	obs_data_set_frames_per_second(settings, "frame_rate", fps, NULL);
#else
	char resolution[32];
	sprintf(resolution, "%dx%d", inputWidth, inputHeight);

	obs_data_set_string(settings, "video_device_id", deviceId);
	obs_data_set_string(settings, "resolution", resolution);
	// Custom resolution
	obs_data_set_int(settings, "res_type", 1);
#endif

	obs_source_update(webcamSource, settings);

	blog(LOG_INFO, "Set webcam to %s", deviceId);

	return 0;
}

// Get recording ready to go - but with no scenes or sources
static int initializeRecording(json_t *obj)
{
	blog(LOG_INFO, "In initializeRecording");

	json_t *inputWidthObj = json_object_get(obj, "inputWidth");
	if (!json_is_integer(inputWidthObj)) {
		fprintf(stderr, "error: inputWidth is not an integer\n");
		return 1;
	}
	int inputWidth = json_integer_value(inputWidthObj);

	json_t *inputHeightObj = json_object_get(obj, "inputHeight");
	if (!json_is_integer(inputHeightObj)) {
		fprintf(stderr, "error: inputHeight is not an integer\n");
		return 1;
	}
	int inputHeight = json_integer_value(inputHeightObj);

	json_t *outputWidthObj = json_object_get(obj, "outputWidth");
	if (!json_is_integer(outputWidthObj)) {
		fprintf(stderr, "error: outputWidth is not an integer\n");
		return 1;
	}
	int outputWidth = json_integer_value(outputWidthObj);

	json_t *outputHeightObj = json_object_get(obj, "outputHeight");
	if (!json_is_integer(outputHeightObj)) {
		fprintf(stderr, "error: outputHeight is not an integer\n");
		return 1;
	}
	int outputHeight = json_integer_value(outputHeightObj);

	json_t *scaledWidthObj = json_object_get(obj, "scaledWidth");
	if (!json_is_integer(scaledWidthObj)) {
		fprintf(stderr, "error: scaledWidth is not an integer\n");
		return 1;
	}
	s_output_width = json_integer_value(scaledWidthObj);

	json_t *scaledHeightObj = json_object_get(obj, "scaledHeight");
	if (!json_is_integer(scaledHeightObj)) {
		fprintf(stderr, "error: scaledHeight is not an integer\n");
		return 1;
	}
	s_output_height = json_integer_value(scaledHeightObj);

	// Optional params to return only a slice of the scaled output
	// Used when returning just the webcam portion of the scene
	s_output_slice_width = -1;
	s_output_slice_height = -1;
	s_output_slice_x = -1;
	s_output_slice_y = -1;
	if (s_output_slice_buffer) {
		free(s_output_slice_buffer);
		s_output_slice_buffer = NULL;
	}
	json_t *scaledSliceWidth = json_object_get(obj, "scaledSliceWidth");
	if (json_is_integer(scaledSliceWidth)) {
		s_output_slice_width = json_integer_value(scaledSliceWidth);
	}
	json_t *scaledSliceHeight = json_object_get(obj, "scaledSliceHeight");
	if (json_is_integer(scaledSliceHeight)) {
		s_output_slice_height = json_integer_value(scaledSliceHeight);
	}
	json_t *scaledSliceX = json_object_get(obj, "scaledSliceX");
	if (json_is_integer(scaledSliceX)) {
		s_output_slice_x = json_integer_value(scaledSliceX);
	}
	json_t *scaledSliceY = json_object_get(obj, "scaledSliceY");
	if (json_is_integer(scaledSliceY)) {
		s_output_slice_y = json_integer_value(scaledSliceY);
	}

	// If we are going to output just a slice, allocate buffer now
	if (s_output_slice_width > 0) {
		fprintf(stderr, "USING SLICE BUFFER\n");
		s_output_slice_buffer = malloc(sizeof(uint8_t) * 4 * s_output_slice_width * s_output_slice_height);
	} else {
		s_output_slice_buffer = NULL;
	}

	struct obs_video_info ovi;
	ovi.adapter = 0;
	ovi.gpu_conversion = false;
#ifndef _WIN32
	ovi.graphics_module = "libobs-opengl";
#else
	ovi.graphics_module = "libobs-d3d11";
#endif
	ovi.fps_num = 30000;
	ovi.fps_den = 1000;
	ovi.base_width = inputWidth;
	ovi.base_height = inputHeight;
	ovi.output_width = outputWidth;
	ovi.output_height = outputHeight;
	ovi.output_format = VIDEO_FORMAT_RGBA;
	ovi.scale_type = OBS_SCALE_BILINEAR;

	// Make sure to remove listener so that video is not considered active
	if (s_raw_output_active) {
		obs_remove_raw_video_callback(receive_video, NULL);
		s_raw_output_active = false;
	}

	blog(LOG_INFO, "Resetting video");
	int rc = obs_reset_video(&ovi);
	blog(LOG_INFO, "Result: %d", rc);

	// Now add raw video callback
	struct video_scale_info info = {0};
	info.format = VIDEO_FORMAT_RGBA;
	info.width = s_output_width;
	info.height = s_output_height;
	obs_add_raw_video_callback(&info, receive_video, NULL);
	s_raw_output_active = true;

	return 0;
}

static const int initializeAudio(json_t *command)
{
	json_t *deviceIdObj = json_object_get(command, "deviceId");
	if (!json_is_string(deviceIdObj)) {
		fprintf(stderr, "error: deviceIdObj is not a string\n");
		return 0;
	}
	const char *deviceId = json_string_value(deviceIdObj);

	json_t *syncOffsetMsObj = json_object_get(command, "syncOffsetMs");
	if (!json_is_integer(syncOffsetMsObj)) {
		fprintf(stderr, "error: syncOffsetMs is not an int\n");
		return 0;
	}
	int syncOffSetMs = json_integer_value(syncOffsetMsObj);

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "device_id", deviceId);
	obs_source_update(audioSource, settings);

	obs_source_set_sync_offset(audioSource,
				   (int64_t)syncOffSetMs * (int64_t)1000000);

	blog(LOG_INFO, "Set audio to %s", deviceId);

	return 0;
}

static const int setAudioDelay(json_t *command)
{
	json_t *syncOffsetMsObj = json_object_get(command, "audioDelayMs");
	if (!json_is_integer(syncOffsetMsObj)) {
		fprintf(stderr, "error: audioDelayMs is not an int\n");
		return 0;
	}
	int audioDelayMs = json_integer_value(syncOffsetMsObj);

	obs_source_set_sync_offset(audioSource,
				   (int64_t)audioDelayMs * (int64_t)1000000);

	blog(LOG_INFO, "Set audio delay to %d ms", audioDelayMs);

	return 0;
}

static const int startRecording(json_t *command)
{
    blog(LOG_INFO, "In startRecording");
    
	json_t *outputFileObj = json_object_get(command, "outputFile");
	if (!json_is_string(outputFileObj)) {
		fprintf(stderr, "error: outputFileObj is not a string\n");
		return 1;
	}
	const char *outputFilePath = json_string_value(outputFileObj);

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "path", outputFilePath);
	fileOutput = obs_output_create("ffmpeg_muxer", "simple_file_output",
				       settings, NULL);
	if (!fileOutput) {
		blog(LOG_ERROR, "ERROR\n");
		return 1;
	}

	obs_set_output_source(1, audioSource);

	// TODO - make this configurable
	// obs_data_t *encoderSettings= obs_data_create();
	// obs_data_set_string(encoderSettings, "rate_control", "CBR");

	encoder = obs_video_encoder_create("obs_x264", "simple_h264_recording", NULL, NULL);
	if (!encoder) {
		blog(LOG_ERROR, "ERROR MAKING ENCODER");
		return 1;
	}
	blog(LOG_INFO, "Created encoder\n");

	audioEncoder = obs_audio_encoder_create(
		"ffmpeg_aac", "simple_aac_recording", NULL, 0, NULL);
	if (!audioEncoder) {
		blog(LOG_ERROR, "ERROR MAKING ENCODER");
	}
	blog(LOG_INFO, "Created audio encoder");

	obs_encoder_set_video(encoder, obs_get_video());
	obs_encoder_set_audio(audioEncoder, obs_get_audio());
	obs_output_set_video_encoder(fileOutput, encoder);
	obs_output_set_audio_encoder(fileOutput, audioEncoder, 0);

	blog(LOG_INFO, "Starting to record");
	if (!obs_output_start(fileOutput)) {
		blog(LOG_ERROR, "Failed to start recording");
		return 1;
	}

    blog(LOG_INFO, "Recording started");

	return 0;
}

static const list_audio_devices(json_t *returnObj)
{
	json_t *array = json_array();
	json_object_set_new(returnObj, "devices", array);

	obs_properties_t *audioProps = obs_source_properties(audioSource);
	obs_property_t *property = obs_properties_first(audioProps);
	while (property != NULL) {
		const char *name = obs_property_name(property);
		enum obs_property_type type = obs_property_get_type(property);
		blog(LOG_INFO, "Property: %s, %d", name, type);

		if (strcmp(name, "device_id") == 0) {
			int numItems = obs_property_list_item_count(property);
			blog(LOG_INFO, "%d items", numItems);
			for (int i = 0; i < numItems; i++) {
				json_t *deviceObj = json_object();
				json_object_set_new(
					deviceObj, "deviceName",
					json_string(obs_property_list_item_name(
						property, i)));
				json_object_set_new(
					deviceObj, "deviceId",
					json_string(
						obs_property_list_item_string(
							property, i)));
				json_array_append(array, deviceObj);

				blog(LOG_INFO, "List item name: %s",
				     obs_property_list_item_name(property, i));
				blog(LOG_INFO, "List item string value: %s",
				     obs_property_list_item_string(property,
								   i));
				blog(LOG_INFO, "List item int value: %d",
				     obs_property_list_item_int(property, i));
				blog(LOG_INFO, "List item float value: %f",
				     obs_property_list_item_float(property, i));
			}
		}

		obs_property_next(&property);
	}
}

static void list_webcam_devices(json_t *returnObj)
{
	json_t *deviceList = obs_source_device_list(webcamSource);

	fprintf(stderr, "Got devicelist");
	fprintf(stderr, "Devices: %s", json_dumps(deviceList, 0));

	json_object_set_new(returnObj, "devices", deviceList);
}

static void list_display_devices(json_t *returnObj)
{
	json_t *deviceList = obs_source_device_list(displaySource);

	fprintf(stderr, "Got devicelist");
	fprintf(stderr, "Devices: %s", json_dumps(deviceList, 0));

	json_object_set_new(returnObj, "devices", deviceList);
}

static int initializeScenes(json_t *obj)
{
	blog(LOG_INFO, "In initializeScenes");

	// Delete any existing scenes
	for (int i = 0; i < numScenes; i++) {
		obs_scene_release(sceneList[i]);
	}
	bfree(sceneList);
	sceneList = NULL;

	json_t *scenes = json_object_get(obj, "scenes");
	numScenes = json_array_size(scenes);

	blog(LOG_INFO, "Creating scenes");

	sceneList = bmalloc(sizeof(obs_scene_t *) * numScenes);
	for (int i = 0; i < numScenes; i++) {
		char *sceneName = malloc(sizeof(char) * 10);
		sprintf(sceneName, "Scene_%d", i);
		obs_scene_t *scene = obs_scene_create(sceneName);

		sceneList[i] = scene;

		json_t *sceneInfo = json_array_get(scenes, i);

		json_t *sources = json_object_get(sceneInfo, "itemSources");
		int numSources = json_array_size(sources);

		blog(LOG_INFO, "Found %d sources", numSources);

		for (int j = 0; j < numSources; j++) {
			json_t *itemSourceInfo = json_array_get(sources, j);

			json_t *sourceTypeObj =
				json_object_get(itemSourceInfo, "type");
			const char *sourceType =
				json_string_value(sourceTypeObj);

			int cropLeft = 0;
			int cropRight = 0;
			int cropTop = 0;
			int cropBottom = 0;
			int x = 0;
			int y = 0;
			float scaleX = 1.0;
			float scaleY = 1.0;

			json_t *cropLeftObj =
				json_object_get(itemSourceInfo, "cropLeft");
			if (cropLeftObj) {
				cropLeft = json_integer_value(cropLeftObj);
			}
			json_t *cropRightObj =
				json_object_get(itemSourceInfo, "cropRight");
			if (cropRightObj) {
				cropRight = json_integer_value(cropRightObj);
			}
			json_t *cropTopObj =
				json_object_get(itemSourceInfo, "cropTop");
			if (cropTopObj) {
				cropTop = json_integer_value(cropTopObj);
			}
			json_t *cropBottomObj =
				json_object_get(itemSourceInfo, "cropBottom");
			if (cropBottomObj) {
				cropBottom = json_integer_value(cropBottomObj);
			}
			json_t *posXObj =
				json_object_get(itemSourceInfo, "x");
			if (posXObj) {
				x = json_integer_value(posXObj);
			}
			json_t *posYObj = json_object_get(itemSourceInfo, "y");
			if (posYObj) {
				y = json_integer_value(posYObj);
			}
			json_t *scaleXObj = json_object_get(itemSourceInfo, "scaleX");
			if (scaleXObj) {
				scaleX = json_real_value(scaleXObj);

				if (scaleX == 0) {
					scaleX = (float) json_integer_value(scaleXObj);
				}
			}
			json_t *scaleYObj =
				json_object_get(itemSourceInfo, "scaleY");
			if (scaleYObj) {
				scaleY = json_real_value(scaleYObj);

				if (scaleY == 0) {
					scaleY = (float) json_integer_value(scaleYObj);
				}
			}

			struct obs_scene_item *item = NULL;

			bool needsTransform = true;

			if (strncmp(sourceType, "webcam", 6) == 0) {
				item = obs_scene_add(scene, webcamSource);
				if (item == NULL) {
					blog(LOG_ERROR,
					     "Could not add scene item");
					continue;
				} else {
					blog(LOG_INFO, "Added webcam to scene");
				}
			} else if (strncmp(sourceType, "display", 7) == 0) {
				item = obs_scene_add(scene, displaySource);
				if (item == NULL) {
					blog(LOG_ERROR,
					     "Could not add monitor item");
					continue;
				} else {
					blog(LOG_INFO,
					     "Added monitor to scene");
				}
			} else if (strncmp(sourceType, "microphone", 10) == 0) {
				item = obs_scene_add(scene, audioSource);
				needsTransform = false;
				if (item == NULL) {
					blog(LOG_ERROR,
					     "Could not add microphone item");
					continue;
				} else {
					blog(LOG_INFO,
					     "Added microphone to scene");
				}
			} else {
					blog(LOG_ERROR, "Unknown type: %s", sourceType);
					continue;
			}

			blog(LOG_INFO, "Checking for transform\n");

			if (needsTransform)
			{
				blog(LOG_INFO, "ScaleX: %f\n", scaleX);
				blog(LOG_INFO, "ScaleY: %f\n", scaleY);

			    item->crop.left = cropLeft;
			    item->crop.top = cropTop;
			    item->crop.right = cropRight;
			    item->crop.bottom = cropBottom;
			    item->pos.x = x;
			    item->pos.y = y;
			    item->scale.x = scaleX;
			    item->scale.y = scaleY;
			    obs_sceneitem_force_update_transform(item);
			}
			else
			{
				blog(LOG_INFO, "No transform needed\n");
			}

		}
	}

	blog(LOG_INFO, "Setting output 0 to scene 0");
	obs_set_output_source(0, obs_scene_get_source(sceneList[0]));
	blog(LOG_INFO, "Set output 0 to scene 0");

	return 0;
}

static int switchToScene(json_t *command)
{
	int sceneNum = 0;

	json_t *sceneNumObj = json_object_get(command, "sceneNum");
	if (sceneNumObj) {
		sceneNum = json_integer_value(sceneNum);
	}

	obs_set_output_source(0, obs_scene_get_source(sceneList[sceneNum]));
}

static const json_t *parse_command(json_t *command)
{
	json_t *returnObj = json_object();

	json_t *actionObj = json_object_get(command, "action");
	if (!json_is_string(actionObj)) {
		fprintf(stderr, "action is not a string");
		json_object_set(returnObj, "error", "action is required");
		return returnObj;
	}
	const char *action = json_string_value(actionObj);

	json_t *actionIdObj = json_object_get(command, "actionId");

	if (!json_is_string(actionIdObj)) {
		fprintf(stderr, "actionId is not a string");
		json_object_set(returnObj, "error", "actionId is required");
		return returnObj;
	}
	const char *actionId = json_string_value(actionIdObj);

	fprintf(stderr, "actionId: %s", actionId);

	json_object_set_new(returnObj, "actionId", json_string(actionId));

	if (strcmp(action, "initialize") == 0) {
		fprintf(stderr, "Initializing");
		if (initialize(command) != 0) {
			fprintf(stderr, "Failed to initialize");
		}
	} else if (strcmp(action, "initializeSingleVideoRecording") == 0) {
		fprintf(stderr, "initSingleVideoRecording");
		if (initializeSingleVideoRecording(command) != 0) {
			fprintf(stderr, "Failed to initialize video");
		}
	} else if (strcmp(action, "initializeAudio") == 0) {
		fprintf(stderr, "initAudio");
		if (initializeAudio(command) != 0) {
			fprintf(stderr, "Failed to initialize audio");
		}
	} else if (strcmp(action, "setAudioDelay") == 0) {
		if (setAudioDelay(command) != 0) {
			fprintf(stderr, "Failed to set audio delay");
		}
	} else if (strcmp(action, "startRecording") == 0) {
		fprintf(stderr, "Starting recording");
		startRecording(command);
	} else if (strcmp(action, "pauseRecording") == 0) {
		fprintf(stderr, "Pausing recording");
		if (!obs_output_pause(fileOutput, true)) {
			fprintf(stderr, "Failed to pause recording");
		}
	} else if (strcmp(action, "resumeRecording") == 0) {
		fprintf(stderr, "Resume recording");
		if (!obs_output_pause(fileOutput, false)) {
			fprintf(stderr, "Failed to resume recording");
		}
	} else if (strcmp(action, "stopRecording") == 0) {
		fprintf(stderr, "Stopping recording");

		signal_handler_t *handler =
			obs_output_get_signal_handler(fileOutput);
		signal_handler_connect(handler, "stop", output_stopped,
				       bstrdup(actionId));

		obs_output_stop(fileOutput);

		// let the stop callback actually return the output
		return NULL;
	} else if (strcmp(action, "shutdown") == 0) {
		fprintf(stderr, "Shutting down");
		obs_set_output_source(0, NULL);
		obs_shutdown();
	} else if (strcmp(action, "listAudioInputDevices") == 0) {
		fprintf(stderr, "Listing Audio Input Devices");
		list_audio_devices(returnObj);
	} else if (strcmp(action, "listWebcamDevices") == 0) {
		fprintf(stderr, "Listing Webcam Devices");
		list_webcam_devices(returnObj);
	} else if (strcmp(action, "listDisplays") == 0) {
		fprintf(stderr, "Listing Display Devices");
		list_display_devices(returnObj);
	} else if (strcmp(action, "startRenderFramesPipe") == 0) {
		json_t *portObj = json_object_get(command, "port");
		if (portObj) {
			int port = json_integer_value(portObj);
			connect_to_local(port);
		}
		start_raw_output();
	} else if (strcmp(action, "stopRenderFramesPipe") == 0) {
		stop_raw_output();
		disconnect_from_local();
	} else if (strcmp(action, "initializeScenes") == 0) {
		fprintf(stderr, "initializeScenes");
		if (initializeScenes(command) != 0) {
			fprintf(stderr, "Failed to initialize scenes");
		}
	} else if (strcmp(action, "initializeRecording") == 0) {
		fprintf(stderr, "initializeRecording");
		if (initializeRecording(command) != 0) {
			fprintf(stderr, "Failed to initialize recording");
		}
	} else if (strcmp(action, "switchToScene") == 0) {
		fprintf(stderr, "switchToScene");
		if (switchToScene(command) != 0) {
			fprintf(stderr, "Failed to switch scenes");
		}
	} else if (strcmp(action, "initializeWebcam") == 0) {
		fprintf(stderr, "initializeWebcam");
		if (initializeWebcam(command) != 0) {
			fprintf(stderr, "Failed to initialize webcam");
		}
	} else if (strcmp(action, "initializeDisplay") == 0) {
		fprintf(stderr, "initializeDisplay");
		if (initializeDisplay(command) != 0) {
			fprintf(stderr, "Failed to initialize display");
		}
	} else {
		fprintf(stderr, "Unrecognized action: %s", action);
	}
	char *str = json_dumps(returnObj, JSON_INDENT(2));

	return returnObj;
}

#ifdef _WIN32
int wmain(int argc, wchar_t *argv_w[])
{
	// Must be per monitor DPI aware
	SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
#else
int main(int argc, char *argv[])
{
#endif
	// TODO - set up a log file properly
	//base_set_log_handler(&null_log_handler, NULL);

	// Loop forever reading one line at a time
	char line[2048];
	ssize_t read;

	while (fgets(line, 2048, stdin)) {
		fprintf(stderr, "Read line %s\n", line);
		if (!line) {
			fprintf(stderr, "Bad line\n");
			continue;
		}

		json_t *root;
		json_error_t error;

		root = json_loads(line, 0, &error);

		if (!root) {
			fprintf(stderr, "error: on line %d: %s\n", error.line,
				error.text);
			continue;
		}

		char *str = json_dumps(root, JSON_INDENT(2));
		if (str) {
			fprintf(stderr, "Read in JSON: %s", str);
			const json_t *returnObj = parse_command(root);

			if (returnObj != NULL) {
				char *returnStr = json_dumps(returnObj, 0);

				// Write newline at the beginning in case someone printed some garbage to stdout
				pthread_mutex_lock(&stdout_mutex);
				fprintf(stderr, "Returning: %s\n", returnStr);
				fprintf(stdout, "\n%s\n", returnStr);
				fflush(stdout);
				pthread_mutex_unlock(&stdout_mutex);

				// Free up all
				free(returnStr);
				free(str);
				free(returnObj);
			}
		}
	}

	printf("Exiting\n");

	return 0;
}
