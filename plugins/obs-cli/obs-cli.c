#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define inline __inline

#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "obs-cli.h"
#include <jansson.h>
#include "util/threading.h"
#include "obs-scene.h"

#ifndef _WIN64
#include <unistd.h>
#else
#define ssize_t long
#endif

#define OBSCLI_LOGFILE_ENV "OBSCLI_LOGFILE_ENV"

static obs_output_t *fileOutput = NULL;
static obs_source_t *audioSource = NULL;
static obs_source_t *displaySource = NULL;
static obs_source_t *webcamSource = NULL;
static obs_encoder_t *encoder = NULL;
static obs_encoder_t *audioEncoder = NULL;
static obs_scene_t *scene = NULL;

static SOCKET sock = INVALID_SOCKET;

// We write to stdout when we get certain events from
// obs and callbacks are not threadsafe, so we coordinate writes.
static pthread_mutex_t stdout_mutex;

static void null_log_handler(int log_level, const char *format, va_list args,
			     void *param)
{
}

static void connect_to_local(int port)
{
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
}

static void receive_video(void *param, struct video_data *frame)
{
	if (sock == INVALID_SOCKET)
	{
		return;
	}

	int frame_size = 1280 * 720 * 4;

	send(sock, frame->data[0], frame_size, 0);
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
	ovi.graphics_module = "libobs-d3d11";
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

	// TODO - pass the source in as a param
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

		#ifdef _WIN32
		obs_data_set_int(displaySettings, "monitor", displayNum);
		#else
		obs_data_set_int(displaySettings, "display", displayNum);
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
		obs_data_set_string(settings, "video_device_id", deviceId);
		obs_data_set_string(settings, "resolution", "1280x720");
		obs_data_set_int(settings, "res_type",1);

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
	ovi.graphics_module = "libobs-d3d11";
	ovi.fps_num = 30000;
	ovi.fps_den = 1000;
	ovi.base_width = inputWidth;
	ovi.base_height = inputHeight;
	ovi.output_width = outputWidth;
	ovi.output_height = outputHeight;
	ovi.output_format = VIDEO_FORMAT_RGBA;
	ovi.scale_type = OBS_SCALE_BILINEAR;

	blog(LOG_INFO, "Resetting video");
	int rc = obs_reset_video(&ovi);
	blog(LOG_INFO, "Result: %d", rc);

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

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "device_id", deviceId);
	obs_source_update(audioSource, settings);

	blog(LOG_INFO, "Set audio to %s", deviceId);

	return 0;
}

static const int startRecording(json_t *command)
{
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

	encoder = obs_video_encoder_create("obs_x264", "simple_h264_recording",
					   NULL, NULL);
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
		fprintf(stderr, "Failed to start recording");
		return 1;
	}

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
		// Hook up the listner here at the last possible minute
		// because if it is added earlier we can't change video size.
		struct video_scale_info info = {0};
		info.format = VIDEO_FORMAT_RGBA;
		obs_add_raw_video_callback(&info, receive_video, NULL);

		json_t *portObj = json_object_get(command, "port");
		if (portObj) {
			int port = json_integer_value(portObj);
			connect_to_local(port);
		}
	}
	char *str = json_dumps(returnObj, JSON_INDENT(2));

	return returnObj;
}

#ifdef _WIN32
long __stdcall WindowProcedure(HWND window, unsigned int msg, WPARAM wp,
			       LPARAM lp)
{
	switch (msg) {
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0L;
	// fall thru
	default:
		return DefWindowProc(window, msg, wp, lp);
	}
}

int wmain(int argc, wchar_t *argv_w[])
{
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

	return 0;
}
