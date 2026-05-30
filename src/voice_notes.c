#define AP_IMPLEMENTATION
#include "apostrophe.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_RECORDINGS 512

typedef enum {
	STATE_IDLE,
	STATE_RECORDING,
	STATE_PLAYING,
	STATE_ERROR
} AppState;

typedef struct {
	char path[512];
	char name[256];
	off_t size;
	time_t mtime;
	int duration_seconds;
	unsigned int sample_rate;
	unsigned int channels;
	unsigned int bits_per_sample;
	unsigned int byte_rate;
	unsigned int data_offset;
	unsigned int data_size;
} Recording;

typedef struct {
	Recording recordings[MAX_RECORDINGS];
	int recording_count;
	int selected;
	int scroll;
	AppState state;
	pid_t record_pid;
	pid_t play_pid;
	int playing_index;
	char playing_path[512];
	int playback_offset_seconds;
	int playback_duration_seconds;
	int playback_paused;
	int playback_paused_elapsed_seconds;
	Uint32 playback_started_at;
	Uint32 record_started_at;
	Uint32 delete_confirm_until;
	int delete_confirm_index;
	char active_path[512];
	char status[128];
	Uint32 status_until;
	const char *output_dir;
	const char *record_device;
	const char *record_rate;
	const char *record_channels;
	const char *record_format;
} App;

static void log_line(const char *fmt, ...)
{
	time_t now = time(NULL);
	struct tm tmv;
	localtime_r(&now, &tmv);
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);

	fprintf(stderr, "[%s] ", stamp);
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fprintf(stderr, "\n");
	fflush(stderr);
}

static const char *env_default(const char *name, const char *fallback)
{
	const char *value = getenv(name);
	return value && value[0] ? value : fallback;
}

static int has_wav_ext(const char *name)
{
	size_t len = strlen(name);
	if (len < 4) return 0;
	return strcasecmp(name + len - 4, ".wav") == 0;
}

static const char *path_basename(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static unsigned int read_le16(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned int read_le32(const unsigned char *p)
{
	return (unsigned int)p[0] |
		((unsigned int)p[1] << 8) |
		((unsigned int)p[2] << 16) |
		((unsigned int)p[3] << 24);
}

static int duration_fallback_seconds(off_t size)
{
	const int bytes_per_second = 48000 * 2;
	if (size <= 44) return 0;
	return (int)((size - 44 + bytes_per_second / 2) / bytes_per_second);
}

static int wav_load_info(Recording *rec)
{
	const char *path = rec->path;
	off_t size = rec->size;
	FILE *fp = fopen(path, "rb");
	if (!fp) return duration_fallback_seconds(size);

	unsigned char header[12];
	if (fread(header, 1, sizeof(header), fp) != sizeof(header) ||
		memcmp(header, "RIFF", 4) != 0 ||
		memcmp(header + 8, "WAVE", 4) != 0) {
		fclose(fp);
		return duration_fallback_seconds(size);
	}

	unsigned int data_size = 0;
	unsigned int data_offset = 0;
	unsigned int byte_rate = 0;
	unsigned int sample_rate = 0;
	unsigned int channels = 0;
	unsigned int bits_per_sample = 0;
	for (;;) {
		unsigned char chunk[8];
		if (fread(chunk, 1, sizeof(chunk), fp) != sizeof(chunk)) break;
		unsigned int chunk_size = read_le32(chunk + 4);
		long chunk_start = ftell(fp);
		if (chunk_start < 0) break;

		if (memcmp(chunk, "fmt ", 4) == 0) {
			unsigned char fmt[16];
			size_t want = chunk_size < sizeof(fmt) ? chunk_size : sizeof(fmt);
			if (fread(fmt, 1, want, fp) == want && want >= 16) {
				channels = read_le16(fmt + 2);
				sample_rate = read_le32(fmt + 4);
				bits_per_sample = read_le16(fmt + 14);
				byte_rate = read_le32(fmt + 8);
				if (byte_rate == 0 && channels > 0 && sample_rate > 0 && bits_per_sample > 0) {
					byte_rate = sample_rate * channels * bits_per_sample / 8;
				}
			}
		} else if (memcmp(chunk, "data", 4) == 0) {
			data_size = chunk_size;
			data_offset = (unsigned int)chunk_start;
		}

		long next = chunk_start + (long)chunk_size + (long)(chunk_size & 1);
		if (fseek(fp, next, SEEK_SET) != 0) break;
		if (byte_rate > 0 && data_size > 0) break;
	}

	fclose(fp);
	rec->sample_rate = sample_rate;
	rec->channels = channels;
	rec->bits_per_sample = bits_per_sample;
	rec->byte_rate = byte_rate;
	rec->data_offset = data_offset;
	rec->data_size = data_size;
	if (byte_rate == 0 || data_size == 0) return duration_fallback_seconds(size);
	int seconds = (int)((data_size + byte_rate / 2) / byte_rate);
	return seconds > 0 ? seconds : 1;
}

static int compare_recordings(const void *a, const void *b)
{
	const Recording *ra = (const Recording *)a;
	const Recording *rb = (const Recording *)b;
	if (ra->mtime > rb->mtime) return -1;
	if (ra->mtime < rb->mtime) return 1;
	return strcmp(rb->name, ra->name);
}

static void set_status(App *app, const char *text)
{
	snprintf(app->status, sizeof(app->status), "%s", text);
	app->status_until = SDL_GetTicks() + 2200;
}

static void clear_delete_confirm(App *app)
{
	app->delete_confirm_until = 0;
	app->delete_confirm_index = -1;
}

static int delete_confirm_active(App *app)
{
	return app->delete_confirm_index == app->selected &&
		SDL_GetTicks() < app->delete_confirm_until;
}

static int playback_active(App *app)
{
	return app->play_pid > 0 || app->playback_paused;
}

static int ensure_dir(const char *path)
{
	if (mkdir(path, 0755) == 0 || errno == EEXIST) return 1;
	log_line("mkdir failed: %s: %s", path, strerror(errno));
	return 0;
}

static void refresh_recordings(App *app, const char *select_path)
{
	app->recording_count = 0;

	DIR *dir = opendir(app->output_dir);
	if (!dir) {
		log_line("opendir failed: %s: %s", app->output_dir, strerror(errno));
		return;
	}

	struct dirent *entry;
	while ((entry = readdir(dir)) && app->recording_count < MAX_RECORDINGS) {
		if (entry->d_name[0] == '.' || !has_wav_ext(entry->d_name)) continue;

		Recording *rec = &app->recordings[app->recording_count];
		snprintf(rec->path, sizeof(rec->path), "%s/%s", app->output_dir, entry->d_name);

		struct stat st;
		if (stat(rec->path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

		snprintf(rec->name, sizeof(rec->name), "%s", entry->d_name);
		rec->size = st.st_size;
		rec->mtime = st.st_mtime;
		rec->duration_seconds = wav_load_info(rec);
		app->recording_count++;
	}
	closedir(dir);

	qsort(app->recordings, app->recording_count, sizeof(Recording), compare_recordings);

	if (app->recording_count <= 0) {
		app->selected = 0;
		app->scroll = 0;
		return;
	}

	int selected = -1;
	if (select_path && select_path[0]) {
		for (int i = 0; i < app->recording_count; i++) {
			if (strcmp(app->recordings[i].path, select_path) == 0) {
				selected = i;
				break;
			}
		}
	}
	if (selected < 0 && app->selected >= 0 && app->selected < app->recording_count) {
		selected = app->selected;
	}
	if (selected < 0) selected = 0;
	app->selected = selected;
	if (app->scroll > app->selected) app->scroll = app->selected;
}

static void show_active_recording(App *app)
{
	if (!app->active_path[0]) return;
	for (int i = 0; i < app->recording_count; i++) {
		if (strcmp(app->recordings[i].path, app->active_path) == 0) {
			app->selected = i;
			return;
		}
	}

	int count = app->recording_count;
	if (count >= MAX_RECORDINGS) count = MAX_RECORDINGS - 1;
	if (count > 0) {
		memmove(&app->recordings[1], &app->recordings[0], (size_t)count * sizeof(app->recordings[0]));
	}

	Recording *rec = &app->recordings[0];
	memset(rec, 0, sizeof(*rec));
	snprintf(rec->path, sizeof(rec->path), "%s", app->active_path);
	snprintf(rec->name, sizeof(rec->name), "%s", path_basename(app->active_path));
	rec->mtime = time(NULL);
	rec->size = 0;
	rec->duration_seconds = 0;
	app->recording_count = count + 1;
	app->selected = 0;
	app->scroll = 0;
}

static void make_recording_path(App *app, char *out, size_t out_size)
{
	time_t now = time(NULL);
	struct tm tmv;
	localtime_r(&now, &tmv);
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
	snprintf(out, out_size, "%s/voice-note-%s.wav", app->output_dir, stamp);
}

static pid_t spawn_process(char *const argv[])
{
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	return pid;
}

static pid_t spawn_playback_process(Recording *rec, int offset_seconds)
{
	if (rec->sample_rate == 0 || rec->channels == 0 || rec->bits_per_sample != 16 ||
		rec->byte_rate == 0 || rec->data_offset == 0) {
		if (offset_seconds > 0) return -1;
		char *argv[] = {
			"aplay",
			rec->path,
			NULL
		};
		return spawn_process(argv);
	}

	unsigned int frame_bytes = rec->channels * rec->bits_per_sample / 8;
	if (frame_bytes == 0) return -1;
	unsigned int skip = rec->data_offset + (unsigned int)offset_seconds * rec->byte_rate;
	skip = rec->data_offset + ((skip - rec->data_offset) / frame_bytes) * frame_bytes;
	if (rec->data_size > 0 && skip >= rec->data_offset + rec->data_size) return -1;

	char rate[16];
	char channels[16];
	snprintf(rate, sizeof(rate), "%u", rec->sample_rate);
	snprintf(channels, sizeof(channels), "%u", rec->channels);

	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		int fd = open(rec->path, O_RDONLY);
		if (fd < 0) _exit(127);
		if (lseek(fd, (off_t)skip, SEEK_SET) < 0) _exit(127);
		if (dup2(fd, STDIN_FILENO) < 0) _exit(127);
		close(fd);
		execlp("aplay", "aplay", "-q", "-f", "S16_LE", "-r", rate, "-c", channels, "-", (char *)NULL);
		_exit(127);
	}
	return pid;
}

static void terminate_child(pid_t pid, const char *label)
{
	if (pid <= 0) return;
	log_line("stopping %s pid=%ld", label, (long)pid);
	kill(pid, SIGTERM);

	Uint32 deadline = SDL_GetTicks() + 800;
	while (SDL_GetTicks() < deadline) {
		pid_t result = waitpid(pid, NULL, WNOHANG);
		if (result == pid) return;
		SDL_Delay(20);
	}

	if (waitpid(pid, NULL, WNOHANG) == 0) {
		log_line("killing stuck %s pid=%ld", label, (long)pid);
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
	}
}

static void reap_playback(App *app)
{
	if (app->play_pid <= 0) return;
	int status = 0;
	pid_t result = waitpid(app->play_pid, &status, WNOHANG);
	if (result == app->play_pid) {
		log_line("aplay exited pid=%ld status=%d", (long)app->play_pid, status);
		app->play_pid = -1;
		if (app->state == STATE_PLAYING) app->state = STATE_IDLE;
		app->playing_index = -1;
		app->playing_path[0] = '\0';
		app->playback_paused = 0;
		app->playback_paused_elapsed_seconds = 0;
		app->status[0] = '\0';
		app->status_until = 0;
	}
}

static void stop_playback(App *app)
{
	if (app->play_pid > 0) {
		terminate_child(app->play_pid, "playback");
		app->play_pid = -1;
	}
	if (app->state == STATE_PLAYING) app->state = STATE_IDLE;
	app->playing_index = -1;
	app->playing_path[0] = '\0';
	app->playback_paused = 0;
	app->playback_paused_elapsed_seconds = 0;
	app->status[0] = '\0';
	app->status_until = 0;
}

static void delete_selected(App *app)
{
	if (app->record_pid > 0 || playback_active(app)) return;
	if (app->recording_count <= 0 || app->selected < 0 || app->selected >= app->recording_count) {
		set_status(app, "No recording selected");
		return;
	}

	Uint32 now = SDL_GetTicks();
	if (app->delete_confirm_index != app->selected || now > app->delete_confirm_until) {
		app->delete_confirm_index = app->selected;
		app->delete_confirm_until = now + 3000;
		return;
	}

	char deleted_path[512];
	snprintf(deleted_path, sizeof(deleted_path), "%s", app->recordings[app->selected].path);
	log_line("delete recording: %s", deleted_path);
	if (unlink(deleted_path) == 0) {
		clear_delete_confirm(app);
		if (app->selected > 0) app->selected--;
		refresh_recordings(app, NULL);
	} else {
		log_line("delete failed: %s: %s", deleted_path, strerror(errno));
		clear_delete_confirm(app);
		app->state = STATE_ERROR;
		set_status(app, "Delete failed");
	}
}

static int playback_elapsed_seconds(App *app)
{
	if (app->playback_paused) return app->playback_paused_elapsed_seconds;
	if (app->play_pid <= 0) return 0;
	int elapsed = app->playback_offset_seconds + (int)((SDL_GetTicks() - app->playback_started_at) / 1000);
	if (app->playback_duration_seconds > 0 && elapsed > app->playback_duration_seconds) {
		elapsed = app->playback_duration_seconds;
	}
	if (elapsed < 0) elapsed = 0;
	return elapsed;
}

static void start_playback_at(App *app, int offset_seconds)
{
	clear_delete_confirm(app);
	if (app->play_pid > 0) {
		stop_playback(app);
	}
	app->playback_paused = 0;
	app->playback_paused_elapsed_seconds = 0;
	if (app->recording_count <= 0 || app->selected < 0 || app->selected >= app->recording_count) {
		set_status(app, "No recordings");
		return;
	}
	if (app->record_pid > 0) return;

	Recording *rec = &app->recordings[app->selected];
	if (offset_seconds < 0) offset_seconds = 0;
	if (rec->duration_seconds > 0 && offset_seconds >= rec->duration_seconds) {
		offset_seconds = rec->duration_seconds - 1;
	}

	log_line("command: aplay '%s' offset=%d", rec->path, offset_seconds);
	pid_t pid = spawn_playback_process(rec, offset_seconds);
	if (pid < 0) {
		log_line("aplay spawn failed: %s", strerror(errno));
		app->state = STATE_ERROR;
		set_status(app, "Playback failed");
		return;
	}
	app->play_pid = pid;
	app->state = STATE_PLAYING;
	app->playing_index = app->selected;
	snprintf(app->playing_path, sizeof(app->playing_path), "%s", rec->path);
	app->playback_offset_seconds = offset_seconds;
	app->playback_duration_seconds = rec->duration_seconds;
	app->playback_started_at = SDL_GetTicks();
	app->status[0] = '\0';
	app->status_until = 0;
}

static void start_playback(App *app)
{
	if (app->play_pid > 0) {
		stop_playback(app);
		return;
	}
	start_playback_at(app, 0);
}

static void seek_playback(App *app, int delta_seconds)
{
	if (!playback_active(app) || app->playing_index < 0 || app->playing_index >= app->recording_count) return;
	app->selected = app->playing_index;
	int offset = playback_elapsed_seconds(app) + delta_seconds;
	start_playback_at(app, offset);
}

static void toggle_playback_pause(App *app)
{
	if (app->playback_paused) {
		if (app->playing_index >= 0 && app->playing_index < app->recording_count) {
			app->selected = app->playing_index;
			start_playback_at(app, app->playback_paused_elapsed_seconds);
		}
	} else {
		if (app->play_pid <= 0) return;
		app->playback_paused_elapsed_seconds = playback_elapsed_seconds(app);
		log_line("pausing playback pid=%ld at=%d", (long)app->play_pid, app->playback_paused_elapsed_seconds);
		terminate_child(app->play_pid, "playback");
		app->play_pid = -1;
		app->playback_paused = 1;
		app->state = STATE_PLAYING;
	}
}

static int file_size_ok(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0) return 0;
	return st.st_size > 44;
}

static void stop_recording(App *app)
{
	if (app->record_pid <= 0) return;

	log_line("stopping recording pid=%ld file='%s'", (long)app->record_pid, app->active_path);
	kill(app->record_pid, SIGINT);

	int status = 0;
	Uint32 deadline = SDL_GetTicks() + 2500;
	while (SDL_GetTicks() < deadline) {
		pid_t result = waitpid(app->record_pid, &status, WNOHANG);
		if (result == app->record_pid) break;
		SDL_Delay(20);
	}
	if (waitpid(app->record_pid, &status, WNOHANG) == 0) {
		kill(app->record_pid, SIGTERM);
		waitpid(app->record_pid, &status, 0);
	}

	app->record_pid = -1;
	app->state = STATE_IDLE;

	if (file_size_ok(app->active_path)) {
		log_line("recording saved: %s", app->active_path);
		refresh_recordings(app, app->active_path);
		app->status[0] = '\0';
		app->status_until = 0;
	} else {
		log_line("recording failed or empty: %s", app->active_path);
		unlink(app->active_path);
		refresh_recordings(app, NULL);
		app->state = STATE_ERROR;
		set_status(app, "Recording failed");
	}
	app->active_path[0] = '\0';
}

static void cancel_recording(App *app)
{
	if (app->record_pid <= 0) return;

	char cancelled_path[512];
	snprintf(cancelled_path, sizeof(cancelled_path), "%s", app->active_path);
	log_line("cancelling recording pid=%ld file='%s'", (long)app->record_pid, cancelled_path);
	kill(app->record_pid, SIGTERM);

	Uint32 deadline = SDL_GetTicks() + 800;
	while (SDL_GetTicks() < deadline) {
		pid_t result = waitpid(app->record_pid, NULL, WNOHANG);
		if (result == app->record_pid) break;
		SDL_Delay(20);
	}
	if (waitpid(app->record_pid, NULL, WNOHANG) == 0) {
		kill(app->record_pid, SIGKILL);
		waitpid(app->record_pid, NULL, 0);
	}

	app->record_pid = -1;
	app->state = STATE_IDLE;
	app->active_path[0] = '\0';
	if (cancelled_path[0]) unlink(cancelled_path);
	refresh_recordings(app, NULL);
	app->status[0] = '\0';
	app->status_until = 0;
}

static void start_recording(App *app)
{
	clear_delete_confirm(app);
	if (app->record_pid > 0) return;
	if (playback_active(app)) stop_playback(app);
	if (!ensure_dir(app->output_dir)) {
		app->state = STATE_ERROR;
		set_status(app, "Output folder failed");
		return;
	}

	make_recording_path(app, app->active_path, sizeof(app->active_path));

	char *argv[] = {
		"arecord",
		"-D", (char *)app->record_device,
		"-f", (char *)app->record_format,
		"-r", (char *)app->record_rate,
		"-c", (char *)app->record_channels,
		app->active_path,
		NULL
	};
	log_line("command: arecord -D %s -f %s -r %s -c %s '%s'",
		app->record_device,
		app->record_format,
		app->record_rate,
		app->record_channels,
		app->active_path);

	pid_t pid = spawn_process(argv);
	if (pid < 0) {
		log_line("arecord spawn failed: %s", strerror(errno));
		app->active_path[0] = '\0';
		app->state = STATE_ERROR;
		set_status(app, "Recording failed");
		return;
	}
	app->record_pid = pid;
	app->record_started_at = SDL_GetTicks();
	app->state = STATE_RECORDING;
	app->status[0] = '\0';
	app->status_until = 0;
	show_active_recording(app);
}

static void format_size(off_t size, char *out, size_t out_size)
{
	if (size >= 1024 * 1024) {
		snprintf(out, out_size, "%.1f MB", (double)size / (1024.0 * 1024.0));
	} else {
		snprintf(out, out_size, "%.0f KB", (double)size / 1024.0);
	}
}

static void format_duration(int seconds, char *out, size_t out_size)
{
	if (seconds < 0) seconds = 0;
	if (seconds >= 3600) {
		snprintf(out, out_size, "%d:%02d:%02d", seconds / 3600, (seconds / 60) % 60, seconds % 60);
	} else {
		snprintf(out, out_size, "%d:%02d", seconds / 60, seconds % 60);
	}
}

static void format_recording_timer(Uint32 elapsed_ms, char *out, size_t out_size)
{
	unsigned int minutes = elapsed_ms / 60000;
	unsigned int seconds = (elapsed_ms / 1000) % 60;
	unsigned int hundredths = (elapsed_ms % 1000) / 10;
	snprintf(out, out_size, "%u:%02u.%02u", minutes, seconds, hundredths);
}

static void move_selection(App *app, int delta)
{
	if (app->record_pid > 0) return;
	if (app->recording_count <= 0) return;
	clear_delete_confirm(app);
	if (playback_active(app)) stop_playback(app);
	app->selected += delta;
	if (app->selected < 0) app->selected = app->recording_count - 1;
	if (app->selected >= app->recording_count) app->selected = 0;
}

static void draw_status(App *app, SDL_Rect content, int margin)
{
	ap_theme *theme = ap_get_theme();
	TTF_Font *font = ap_get_font(AP_FONT_SMALL);
	if (!font) return;

	char state_text[128] = "";
	Uint32 now = SDL_GetTicks();
	if (app->status[0] && now < app->status_until) {
		snprintf(state_text, sizeof(state_text), "%s", app->status);
	}
	if (!state_text[0]) return;

	int text_h = TTF_FontHeight(font);
	int y = content.y + content.h - text_h - AP_S(8);
	ap_color color = app->state == STATE_RECORDING ? theme->accent : theme->hint;
	int x = margin;
	if (app->state == STATE_RECORDING) {
		int dot = AP_DS(6);
		ap_draw_circle(x + dot / 2, y + text_h / 2, dot / 2, color);
		x += dot + AP_DS(4);
	}
	ap_draw_text(font, state_text, x, y, color);
}

static void draw_playing_icon(int x, int y, int h)
{
	Uint32 phase = (SDL_GetTicks() / 180) % 3;
	int bar_w = AP_DS(2);
	int gap = AP_DS(2);
	int min_h = AP_DS(6);
	int max_h = h - AP_DS(8);
	if (max_h < min_h) max_h = min_h;
	ap_color green = {36, 216, 111, 255};

	for (int i = 0; i < 3; i++) {
		int step = (int)((phase + (Uint32)i) % 3);
		int bar_h = min_h + (max_h - min_h) * (step + 1) / 3;
		int bx = x + i * (bar_w + gap);
		int by = y + (h - bar_h) / 2;
		ap_draw_rect(bx, by, bar_w, bar_h, green);
	}
}

static void draw_pause_icon(int x, int y, int h)
{
	ap_color green = {36, 216, 111, 255};
	int bar_w = AP_DS(3);
	int gap = AP_DS(3);
	int bar_h = h - AP_DS(6);
	if (bar_h < AP_DS(8)) bar_h = AP_DS(8);
	int by = y + (h - bar_h) / 2;
	ap_draw_rect(x, by, bar_w, bar_h, green);
	ap_draw_rect(x + bar_w + gap, by, bar_w, bar_h, green);
}

static void draw_recording_icon(int x, int y, int h)
{
	ap_color red = {226, 50, 82, 255};
	int r = AP_DS(4);
	ap_draw_circle(x + r, y + h / 2, r, red);
}

static void render(App *app)
{
	ap_theme *theme = ap_get_theme();
	TTF_Font *item_font = ap_get_font(AP_FONT_LARGE);
	TTF_Font *meta_font = ap_get_font(AP_FONT_SMALL);
	if (!theme || !item_font || !meta_font) return;

	int screen_w = ap_get_screen_width();
	SDL_Rect content = ap_get_content_rect(false, true, false);
	int margin = AP_DS(ap__g.device_padding);
	int pill_h = AP_DS(AP__PILL_SIZE);
	int pill_pad = AP_DS(AP__BUTTON_PADDING);
	int row_gap = 0;
	int list_h = content.h - AP_DS(14);
	int rows_visible = list_h / (pill_h + row_gap);
	if (rows_visible < 1) rows_visible = 1;

	if (app->selected < app->scroll) app->scroll = app->selected;
	if (app->selected >= app->scroll + rows_visible) app->scroll = app->selected - rows_visible + 1;
	if (app->scroll < 0) app->scroll = 0;

	int available_w = screen_w - margin * 2;
	if (app->recording_count > rows_visible) available_w -= AP_S(12);
	if (available_w < AP_DS(80)) available_w = AP_DS(80);

	ap_draw_background();

	if (app->recording_count <= 0) {
		const char *empty = "NO RECORDINGS.";
		int tw = ap_measure_text(item_font, empty);
		int th = TTF_FontHeight(item_font);
		ap_draw_text(item_font,
			empty,
			(screen_w - tw) / 2,
			content.y + (content.h - th) / 2,
			theme->hint);
	} else {
		for (int row = 0; row < rows_visible; row++) {
			int idx = app->scroll + row;
			if (idx >= app->recording_count) break;

			Recording *rec = &app->recordings[idx];
			int y = content.y + row * (pill_h + row_gap);

			int is_selected = idx == app->selected;
			int is_recording = app->record_pid > 0 && app->active_path[0] && strcmp(rec->path, app->active_path) == 0;
			int is_playing = playback_active(app) && app->playing_path[0] && strcmp(rec->path, app->playing_path) == 0;

			off_t display_size = rec->size;
			int display_duration = rec->duration_seconds;
			if (is_recording) {
				struct stat st;
				if (stat(rec->path, &st) == 0) display_size = st.st_size;
				display_duration = (int)((SDL_GetTicks() - app->record_started_at) / 1000);
				rec->size = display_size;
				rec->duration_seconds = display_duration;
			}

			char duration[32];
			char size[32];
			char right_text[96];
			Uint32 now = SDL_GetTicks();
			int show_delete_hint = is_selected && delete_confirm_active(app);
			if (show_delete_hint) {
				snprintf(right_text, sizeof(right_text), "CONFIRM DELETE");
			} else if (is_recording) {
				format_recording_timer(now - app->record_started_at, right_text, sizeof(right_text));
			} else if (is_playing) {
				char elapsed[32];
				char total[32];
				format_duration(playback_elapsed_seconds(app), elapsed, sizeof(elapsed));
				format_duration(app->playback_duration_seconds > 0 ? app->playback_duration_seconds : display_duration, total, sizeof(total));
				snprintf(right_text, sizeof(right_text), "%s / %s", elapsed, total);
			} else {
				format_duration(display_duration, duration, sizeof(duration));
				format_size(display_size, size, sizeof(size));
				snprintf(right_text, sizeof(right_text), "%s  %s", duration, size);
			}
			int right_w = ap_measure_text(meta_font, right_text);
			int right_gap = AP_DS(12);
			int name_area_w = available_w;
			if (right_w > 0 && available_w - right_w - right_gap >= AP_DS(120)) {
				name_area_w -= right_w + right_gap;
			} else {
				right_w = 0;
			}

			int icon_w = 0;
			int icon_gap = 0;
			if (is_playing) {
				icon_w = AP_DS(14);
				icon_gap = AP_DS(7);
			} else if (is_recording) {
				icon_w = AP_DS(8);
				icon_gap = AP_DS(7);
			}
			int text_max_w = ap__max(AP_DS(40), name_area_w - pill_pad * 2 - icon_w - icon_gap);
			int visible_name_w = ap_measure_text_ellipsized(item_font, rec->name, text_max_w);

			if (is_selected) {
				int selected_w = visible_name_w + icon_w + icon_gap + pill_pad * 2;
				if (selected_w > name_area_w) selected_w = name_area_w;
				ap_draw_pill(margin, y, selected_w, pill_h, theme->highlight);
			}

			ap_color text_color = is_selected ? theme->highlighted_text : theme->text;
			int name_y = y + (pill_h - TTF_FontHeight(item_font)) / 2;
			int text_x = margin + pill_pad;
			ap_draw_text_ellipsized(item_font,
				rec->name,
				text_x,
				name_y,
				text_color,
				text_max_w);
			if (is_playing) {
				if (app->playback_paused) {
					draw_pause_icon(text_x + visible_name_w + icon_gap, y + AP_DS(8), pill_h - AP_DS(16));
				} else {
					draw_playing_icon(text_x + visible_name_w + icon_gap, y + AP_DS(8), pill_h - AP_DS(16));
				}
			} else if (is_recording) {
				draw_recording_icon(text_x + visible_name_w + icon_gap, y + AP_DS(8), pill_h - AP_DS(16));
			}

			if (right_w > 0) {
				ap_color meta_color = show_delete_hint ? theme->text : theme->hint;
				int meta_x = margin + name_area_w + right_gap;
				ap_draw_text(meta_font,
					right_text,
					meta_x,
					y + (pill_h - TTF_FontHeight(meta_font)) / 2,
					meta_color);
			}
		}
	}

	if (app->recording_count > rows_visible) {
		ap_draw_scrollbar(screen_w - margin - AP_S(6),
			content.y,
			content.h,
			rows_visible,
			app->recording_count,
			app->scroll);
	}

	draw_status(app, content, margin);

	ap_footer_item footer[4];
	int footer_count = 0;
	if (delete_confirm_active(app)) {
		footer[footer_count++] = (ap_footer_item){ .button = AP_BTN_B, .label = "CANCEL", .is_confirm = true };
		footer[footer_count++] = (ap_footer_item){ .button = AP_BTN_A, .label = "CONFIRM", .is_confirm = true };
	} else if (app->record_pid > 0) {
		footer[footer_count++] = (ap_footer_item){ .button = AP_BTN_B, .label = "CANCEL" };
	} else {
		footer[footer_count++] = (ap_footer_item){ .button = AP_BTN_B, .label = "BACK" };
	}
	if (app->record_pid > 0) {
		footer[footer_count++] = (ap_footer_item){ .button = AP_BTN_A, .label = "FINISH", .is_confirm = true };
	} else if (!delete_confirm_active(app)) {
		footer[footer_count++] = (ap_footer_item){ .button = AP_BTN_X, .label = "REC" };
		footer[footer_count++] = (ap_footer_item){ .button = AP_BTN_Y, .label = playback_active(app) ? "STOP" : "DELETE", .is_confirm = true };
		footer[footer_count++] = (ap_footer_item){ .button = AP_BTN_A, .label = playback_active(app) ? (app->playback_paused ? "RESUME" : "PAUSE") : "PLAY", .is_confirm = true };
	}
	ap_draw_footer(footer, footer_count);
}

static int handle_input(App *app, const ap_input_event *ev)
{
	if (ev->button == AP_BTN_X) {
		if (delete_confirm_active(app)) return 1;
		if (ev->pressed && app->record_pid <= 0) {
			start_recording(app);
		}
		return 1;
	}

	if (!ev->pressed) return 1;

	switch (ev->button) {
	case AP_BTN_UP:
		move_selection(app, -1);
		break;
	case AP_BTN_DOWN:
		move_selection(app, 1);
		break;
	case AP_BTN_LEFT:
		seek_playback(app, -5);
		break;
	case AP_BTN_RIGHT:
		seek_playback(app, 5);
		break;
	case AP_BTN_A:
		if (delete_confirm_active(app)) {
			delete_selected(app);
		} else if (app->record_pid > 0) {
			stop_recording(app);
		} else if (playback_active(app)) {
			toggle_playback_pause(app);
		} else if (app->record_pid <= 0) {
			start_playback(app);
		}
		break;
	case AP_BTN_Y:
		if (playback_active(app)) {
			stop_playback(app);
		} else if (!delete_confirm_active(app)) {
			delete_selected(app);
		}
		break;
	case AP_BTN_B:
		if (delete_confirm_active(app)) {
			clear_delete_confirm(app);
			break;
		}
		if (app->record_pid > 0) {
			cancel_recording(app);
			break;
		}
		if (playback_active(app)) stop_playback(app);
		return 0;
	default:
		break;
	}
	return 1;
}

int main(void)
{
	App app;
	memset(&app, 0, sizeof(app));
	app.selected = 0;
	app.delete_confirm_index = -1;
	app.record_pid = -1;
	app.play_pid = -1;
	app.playing_index = -1;
	app.playback_paused = 0;
	app.playback_paused_elapsed_seconds = 0;
	app.state = STATE_IDLE;
	app.output_dir = env_default("VOICE_NOTES_OUTPUT_DIR", "/mnt/SDCARD/Recordings");
	app.record_device = env_default("VOICE_NOTES_DEVICE", "hw:0,0");
	app.record_rate = env_default("VOICE_NOTES_RATE", "48000");
	app.record_channels = env_default("VOICE_NOTES_CHANNELS", "1");
	app.record_format = env_default("VOICE_NOTES_FORMAT", "S16_LE");

	log_line("Voice Notes starting");
	log_line("output_dir=%s device=%s rate=%s channels=%s format=%s",
		app.output_dir,
		app.record_device,
		app.record_rate,
		app.record_channels,
		app.record_format);

	ap_config cfg = {0};
	cfg.window_title = "Voice Notes";
	cfg.is_nextui = true;
	cfg.cpu_speed = AP_CPU_SPEED_MENU;
#if !AP_PLATFORM_IS_DEVICE
	cfg.font_path = "third_party/apostrophe/res/font.ttf";
	cfg.disable_background = true;
#endif

	if (ap_init(&cfg) != AP_OK) {
		log_line("ap_init failed");
		return 1;
	}

	ensure_dir(app.output_dir);
	refresh_recordings(&app, NULL);

	int running = 1;
	while (running) {
		reap_playback(&app);

		ap_input_event ev;
		while (ap_poll_input(&ev)) {
			running = handle_input(&app, &ev);
			if (!running) break;
		}

		render(&app);

		if (app.state == STATE_RECORDING || app.state == STATE_PLAYING ||
			(app.status[0] && SDL_GetTicks() < app.status_until)) {
			ap_request_frame_in(250);
		}
		ap_present();
	}

	if (app.record_pid > 0) stop_recording(&app);
	if (playback_active(&app)) stop_playback(&app);
	ap_quit();
	log_line("Voice Notes exit");
	return 0;
}
