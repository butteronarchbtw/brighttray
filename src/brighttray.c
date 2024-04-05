#include <libnotify/notification.h>
#include <pthread.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/types.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include <systemd/sd-bus.h>
#include <libnotify/notify.h>

#include <gtk/gtk.h>

#define BACKLIGHT_DIR "/sys/class/backlight"
#define APP_NAME "backlight-tray"

#define MAX_EVENTS 1
#define LEN_NAME 1024
#define EVENT_SIZE  ( sizeof (struct inotify_event) )
#define BUF_LEN     ( MAX_EVENTS * ( EVENT_SIZE + LEN_NAME ))

static int init_backlight_backend();
static int current_brightness();
static int max_brightness();
static int percentage();
static int set_brightness(int value);

static int bl_dir_get_int(char *path);
static char *get_full_device_path();

static int systemd_set_brightness(char *device, int value);

static void create_tray_icon();
static void status_icon_on_scroll_event(GtkStatusIcon *status_icon, GdkEventScroll *event, gpointer user_data);
static void update_status_icon();
static void set_status_icon_info(gchar *name, char *tt_text);
static GdkPixbuf *get_icon_pixbuf(int size);
static void quit_gtk();

static void *query_ino(void *args);
static void process_ino_event(int fd);

static struct config {
	GtkStatusIcon *gtk_icon;
	int icon_size;
	gchar *icon_name;
	int bl_dir_fd;
	int ino_fd;
	char *device;
	int stepsize;
	int cap_at_10;
	NotifyNotification *notif;
} config = {
	NULL,
	0,
	NULL,
	-1,
	-1,
	NULL,
	10,
	1,
	NULL
};

static int init_backlight_backend() {
	int dfd, fd; // directory descriptor, file descriptor to an underlying
	DIR *dir; // stream of directories
	struct dirent *dp; // single entry in stream of directories

	if ((dfd = open(BACKLIGHT_DIR, O_RDONLY)) == -1) {
		return -1;	
	}

	// find first directory in the directory containing all the backlight sources
	if ((dir = fdopendir(dfd)) == NULL) {
		close(dfd);
		return -1;
	}


	// first and second dirents are . and ..
	if ((dp = readdir(dir)) == NULL ||
			(dp = readdir(dir)) == NULL ||
			(dp = readdir(dir)) == NULL ||
			(fd = openat(dfd, dp->d_name, O_PATH)) == -1) {
		fd = -1;
	}

	config.device = malloc(strlen(dp->d_name));
	strcpy(config.device, dp->d_name);

	close(dfd);
	closedir(dir);
	return fd;
}

static int current_brightness() {
	return bl_dir_get_int("brightness");
}

static int max_brightness() {
	return bl_dir_get_int("max_brightness");
}

static int percentage() {
	int c = current_brightness();
	int m = max_brightness();

	return (double) c / m * 100;
}

static int set_brightness(int value) {
	return systemd_set_brightness(config.device, value);
}

static int bl_dir_get_int(char *path) {
	int bn_fd = openat(config.bl_dir_fd, path, O_RDONLY);
	FILE *bn = fdopen(bn_fd, "r");

	int v = -1;
	fscanf(bn, "%d", &v);

	fclose(bn);
	close(bn_fd);
	return v;
}

static char *get_full_device_path() {
	char *dev = malloc(strlen(config.device) + 1 + strlen(BACKLIGHT_DIR));
	strcpy(dev, BACKLIGHT_DIR);
	strcat(dev, "/");
	strcat(dev, config.device);
	return dev;
}

static int systemd_set_brightness(char *device, int value) {
	sd_bus *bus = NULL;
	int r = sd_bus_default_system(&bus);
	if (r < 0) {
		fprintf(stderr, "couldn't connect to system bus: %s\n", strerror(-r));
		return 0;
	}

	r = sd_bus_call_method(bus,
			"org.freedesktop.login1",
			"/org/freedesktop/login1/session/auto",
			"org.freedesktop.login1.Session",
			"SetBrightness",
			NULL,
			NULL,
			"ssu",
			"backlight",
			device,
			value
			);

	if(r < 0) {
		fprintf(stderr, "Failed to set brightness: %s\n", strerror(-r));
		return 0;
	}

	sd_bus_unref(bus);

	return 1;
}

static void create_tray_icon() {
	config.gtk_icon = gtk_status_icon_new ();
	config.icon_name = g_strdup("");
	config.icon_size = 32;

	gtk_status_icon_set_visible (config.gtk_icon, TRUE);
	update_status_icon();

	g_signal_connect(G_OBJECT(config.gtk_icon), "scroll_event", G_CALLBACK(status_icon_on_scroll_event), NULL);
}

static void status_icon_on_scroll_event(GtkStatusIcon *status_icon, GdkEventScroll *event, gpointer user_data) {
	switch(event->direction) {
		case GDK_SCROLL_UP:
			{
				int max = max_brightness();
				int ten_percent = 0.1 * max;
				int curr = current_brightness();
				if (curr <= max - ten_percent) {
					set_brightness(curr + ten_percent);
				}
			}
			break;
		case GDK_SCROLL_DOWN:
			{
				int ten_percent = 0.1 * max_brightness();
				int curr = current_brightness();
				if(curr >= 0 + ten_percent && !config.cap_at_10) {
					set_brightness(current_brightness() - ten_percent);
				} else if(curr > 0 + ten_percent) {
					set_brightness(current_brightness() - ten_percent);
				}
			}
			break;
		case GDK_SCROLL_LEFT:
		case GDK_SCROLL_RIGHT:
		case GDK_SCROLL_SMOOTH:
			break;
	}
}

static void update_status_icon() {
	int per = percentage();
	char *icon_name;
	if(per <= 20) {
		icon_name = "weather-few-clouds-night";
	} else if (per > 20 && per <= 50) {
		icon_name = "weather-clear-night";
	} else if (per > 50 && per <= 70) {
		icon_name = "weather-few-clouds";
	} else {
		icon_name = "weather-clear";
	}

	char per_str[10];
	sprintf(per_str, "%d%%", per);
	set_status_icon_info(icon_name, per_str);

	notify_notification_update(config.notif, per_str, NULL, NULL);
	GdkPixbuf *pix = get_icon_pixbuf(16);
	notify_notification_set_icon_from_pixbuf(config.notif, pix);
	notify_notification_show(config.notif, NULL);
}

static void set_status_icon_info(gchar *name, char *tt_text) {
	if(config.icon_name != NULL) {
		g_free(config.icon_name);
	}
	config.icon_name = g_strdup(name);
	GdkPixbuf *pix = get_icon_pixbuf(config.icon_size);
	gtk_status_icon_set_from_pixbuf (config.gtk_icon, pix);
	gtk_status_icon_set_tooltip_text (config.gtk_icon, tt_text);
}

static GdkPixbuf *get_icon_pixbuf(int size) {
	return gtk_icon_theme_load_icon (
		gtk_icon_theme_get_default(),
		config.icon_name,
		size,
		GTK_ICON_LOOKUP_USE_BUILTIN,
		NULL
	);
}

static void quit_gtk() {
	gtk_main_quit();
}

static void *query_ino(void *args) {
	while(1) {
		process_ino_event(config.ino_fd);
	}
}

static void process_ino_event(int fd) {
	char buffer[BUF_LEN];
	int length, i = 0;
	length = read(fd, buffer, BUF_LEN);  

	if (length < 0) {
		fprintf(stderr, "reading inotify fd did not work\n");
		fflush(stderr);
	}

	if(i < length) {
		struct inotify_event *event = (struct inotify_event *) &buffer[i];
		if(event->len) {
			if(event->mask & IN_MODIFY && strcmp(event->name, "brightness") == 0) {
				update_status_icon();
			}
		}
	}
}


int main(int argc, char **argv) {

	gtk_init(&argc, &argv);

	config.bl_dir_fd = init_backlight_backend();
	if (config.bl_dir_fd == -1 || config.device == NULL) {
		fprintf(stderr, "Failed to retrieve device information\n");
		return -1;
	}

	notify_init(APP_NAME);
	config.notif = notify_notification_new("", "", NULL);
	create_tray_icon();

	config.ino_fd = inotify_init();
	if(config.ino_fd < 0) {
		fprintf(stderr, "Failed to setup listener\n");
		return -1;
	}

	char *path = get_full_device_path();
	int ino_wd = inotify_add_watch(config.ino_fd, path, IN_MODIFY);
	if (ino_wd == -1) {
		fprintf(stderr, "Failed to setup watchdog\n");
		return -1;
	} else {
#ifdef VERBOSE
		printf("watching %s\n", path);
#endif
	}

	pthread_t ino_thread;
	pthread_create(&ino_thread, NULL, &query_ino, (void *) &config);

	signal(SIGINT, &quit_gtk);
	signal(SIGTERM, &quit_gtk);
	gtk_main();

	// CLEANUP
#ifdef VERBOSE
	printf("CLEANUP IN PROGRESS\n");
#endif
	pthread_cancel(ino_thread);
	inotify_rm_watch(config.ino_fd, ino_wd);
	free(path);
	free(config.device);
	g_free(config.icon_name);
	close(config.ino_fd);
	close(config.bl_dir_fd);
	notify_uninit();
#ifdef VERBOSE
	printf("CLEANUP DONE\n");
#endif
	return 0;
}
