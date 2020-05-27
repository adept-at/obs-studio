#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#define inline __inline

#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "obs-cli.h"
#include <jansson.h>

#define OBSCLI_LOGFILE_ENV "OBSCLI_LOGFILE_ENV"

static obs_output_t *fileOutput = NULL;

static void null_log_handler(int log_level, const char *format, va_list args,
			     void *param)
{
}

static int initialize(json_t* obj)
{
	blog(LOG_INFO, "Starting OBS!");

	json_t* pluginDirObj = json_object_get(obj, "pluginDir");
	if(!json_is_string(pluginDirObj))
    {
        fprintf(stderr, "error: pluginDir is not a string\n");
        return 0;
    }
	const char* pluginDir= json_string_value(pluginDirObj);

	json_t* exeDirObj = json_object_get(obj, "exeDir");
	if(!json_is_string(exeDirObj))
    {
        fprintf(stderr, "error: exeDir is not a string\n");
        return 0;
    }
	const char* exeDir= json_string_value(exeDirObj);
	obs_set_executable_path(exeDir);

	json_t* dataDirObj = json_object_get(obj, "dataDir");
	if(!json_is_string(dataDirObj))
    {
        fprintf(stderr, "error: dataDir is not a string\n");
        return 0;
    }
	const char* dataDir= json_string_value(dataDirObj);
	obs_add_data_path(dataDir);

	json_t* outputFileObj = json_object_get(obj, "outputFile");
	if(!json_is_string(outputFileObj))
    {
        fprintf(stderr, "error: outputFileObj is not a string\n");
        return 0;
    }
	const char* outputFilePath = json_string_value(outputFileObj);

	if (!obs_startup("en", ".", NULL)) {
		blog(LOG_ERROR, "Failed to start");
		return 1;
	} else {
		blog(LOG_INFO, "Started OBS");
	}

	struct obs_video_info ovi;
	ovi.adapter = 0;
	ovi.gpu_conversion = false;
	ovi.graphics_module = "libobs-opengl";
	ovi.fps_num = 30000;
	ovi.fps_den = 1000;
	ovi.base_width = 5120;
	ovi.base_height = 2880;
	ovi.output_width = 1280;
	ovi.output_height = 720;
	ovi.output_format = VIDEO_FORMAT_RGBA;
	ovi.scale_type = OBS_SCALE_BILINEAR;

	blog(LOG_INFO, "Resetting video");
	int rc = obs_reset_video(&ovi);
	blog(LOG_INFO, "Result: %d", rc);

	blog(LOG_INFO, "Reset video");

	struct obs_audio_info ai;
	ai.samples_per_sec = 44100;
	ai.speakers = SPEAKERS_MONO;
	obs_reset_audio(&ai);
	blog(LOG_INFO, "Reset audio");

	obs_add_module_path(pluginDir, pluginDir);

	blog(LOG_INFO, "Loading modules");
	obs_load_all_modules();
	obs_post_load_modules();
	blog(LOG_INFO, "Done loading modules");

	obs_encoder_t *encoder = obs_video_encoder_create(
		"obs_x264", "simple_h264_recording", NULL, NULL);
	if (!encoder) {
		blog(LOG_ERROR, "ERROR MAKING ENCODER");
		return 1;
	}

	blog(LOG_INFO, "Created encoder\n");

	obs_encoder_t *audioEncoder = obs_audio_encoder_create(
		"CoreAudio_AAC", "simple_aac_recording", NULL, 0, NULL);
	if (!audioEncoder) {
		blog(LOG_ERROR, "ERROR MAKING ENCODER");
	}

	blog(LOG_INFO, "Created audio encoder");

	obs_source_t *source = obs_source_create("display_capture",
						 "Display Capture", NULL, NULL);
	if (!source) {
		blog(LOG_ERROR, "Unable to create source");
		return 1;
	} else {
		blog(LOG_INFO, "Created display capture!");
	}

	obs_source_t *audioSource = obs_source_create("coreaudio_input_capture", "Microphone", NULL, NULL);
	if (!audioSource) {
		blog(LOG_ERROR, "Unable to create audio source");
		return 1;
	} else {
		blog(LOG_INFO, "Created audio source");
	}

	fileOutput = obs_output_create(
		"ffmpeg_muxer", "simple_file_output", NULL, NULL);
	if (!fileOutput) {
		blog(LOG_ERROR, "ERROR\n");
		return 1;
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "path", outputFilePath);
	obs_output_update(fileOutput, settings);

	// obs_scene_t *scene = obs_scene_create("Test");

	obs_set_output_source(0, source);
	obs_set_output_source(1, audioSource);

	obs_encoder_set_video(encoder, obs_get_video());
	obs_encoder_set_audio(audioEncoder, obs_get_audio());
	obs_output_set_video_encoder(fileOutput, encoder);
	obs_output_set_audio_encoder(fileOutput, audioEncoder, 0);
}

static const json_t* parse_command(json_t* command)
{
	json_t* actionObj = json_object_get(command, "action");

	if(!json_is_string(actionObj))
    {
        fprintf(stderr, "error: action is not a string\n");
        return NULL;
    }

	const char* action = json_string_value(actionObj);

	if (strcmp(action, "initialize") == 0) {
		fprintf(stderr, "Initializing");
		if (!initialize(command)) {
			fprintf(stderr, "Failed to initialize");
		}
	} else if (strcmp(action, "startRecording") == 0) {
		fprintf(stderr, "Starting recording");
		if (!obs_output_start(fileOutput)) {
			fprintf(stderr, "Failed to start recording");
		}
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
		obs_output_stop(fileOutput);
	} else if (strcmp(action, "shutdown") == 0) {
		fprintf(stderr, "Shutting down");
		obs_set_output_source(0, NULL);
		obs_shutdown();
	}

	return NULL;
}

#ifdef _WIN32
int wmain(int argc, wchar_t *argv_w[])
#else
int main(int argc, char *argv[])
#endif
{
	// TODO - set up a log file properly
	//base_set_log_handler(&null_log_handler, NULL);


	// Loop forever reading one line at a time
	char *line = NULL;
	size_t len = 0;
	ssize_t read;

	while ((read = getline(&line, &len, stdin)) != -1) {
		fprintf(stderr, "Read line %s\n", line);
		if (!line) {
			fprintf(stderr, "Bad line\n");
			continue;
		}

		json_t *root;
    	json_error_t error;

		root = json_loads(line, 0, &error);

		if (line) {
			free(line);
			line = NULL;
		}

		if (!root) {
			fprintf(stderr, "error: on line %d: %s\n", error.line, error.text);
			continue;
		}

		char* str = json_dumps(root, JSON_INDENT(2));
		if (str) {
			fprintf(stderr, "Read in JSON: %s", str);
			parse_command(root);
			// Write newline at the beginning in case someone printed
			// some garbage to stdout
			fprintf(stdout, "\n{ \"result\": \"success\" }\n");
			free(str);
		}
	}

	return 0;
}
