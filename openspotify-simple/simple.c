#ifdef _WIN32
#include <windows.h>
#else
#include <libgen.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#endif
#include <stdio.h>

#include "despotify-appkey.h"
#include "debug.h"

// The one and only entrypoint to the libspotify API
#include <spotify/api.h>


/* --- Functions --- */
void session_ready(sp_session *session);
void session_terminated(void);


/* --- Data --- */
int g_exit_code = -1;
#ifdef _WIN32
static HANDLE g_main_thread = (HANDLE)0;  /* -1 is a valid HANDLE, 0 is not */
static HANDLE notifyEvent;
#else
static pthread_t g_main_thread = (pthread_t)-1;
#endif


/* ------------------------  BEGIN SESSION CALLBACKS  ---------------------- */
static void SP_CALLCONV connection_error(sp_session *session, sp_error error)
{
    DSFYDEBUG("CALLBACK: connection_error(session=%p, error=%d)\n", session, error);
    fprintf(stderr, "connection to Spotify failed: %s\n",
                    sp_error_message(error));
    g_exit_code = 5;
}

static void SP_CALLCONV logged_in(sp_session *session, sp_error error) {
    sp_user *me;
    const char *my_name;

    DSFYDEBUG("CALLBACK: logged_in(session=%p, error=%d)\n", session, error);
    if (SP_ERROR_OK != error) {
        fprintf(stderr, "failed to log in to Spotify: %s\n",
                        sp_error_message(error));
        g_exit_code = 4;
        return;
    }

    // Let us print the nice message...
    me = sp_session_user(session);
    my_name = (sp_user_is_loaded(me)? sp_user_display_name(me): sp_user_canonical_name(me));

    printf("Logged in to Spotify as user %s\n", my_name);

    DSFYDEBUG("CALLBACK: logged_in() calling session_ready()\n");
    session_ready(session);
}


static void SP_CALLCONV logged_out(sp_session *session) {
	DSFYDEBUG("CALLBACK: logged_in(session=%p), g_exit_code=%d\n", session, g_exit_code);
	if (g_exit_code < 0)
		g_exit_code = 0;
}


static void SP_CALLCONV notify_main_thread(sp_session *session) {
	DSFYDEBUG("CALLBACK: notify_main_thread(session=%p), will notify sp_process_event()\n", session);

#ifdef _WIN32
	if(PulseEvent(notifyEvent) == 0) {
		DSFYDEBUG("PulseEvent(notifyEvent) failed with error %d\n", GetLastError());
	}
#else
	pthread_kill(g_main_thread, SIGIO);
#endif

}


static void SP_CALLCONV log_message(sp_session *session, const char *data) {
    DSFYDEBUG("CALLBACK: log_message(session=%p, data=%s)\n", session, data);
    fprintf(stderr, "log_message: %s\n", data);
}


static void SP_CALLCONV metadata_updated(sp_session *session) {
	DSFYDEBUG("CALLBACK: metadata_updated(session=%p)\n", session);
}


static sp_session_callbacks g_callbacks = {
    logged_in,
    logged_out,
    metadata_updated,
    connection_error,
    NULL,
    notify_main_thread,
    NULL,
    NULL,
    log_message
};
/* -------------------------  END SESSION CALLBACKS  ----------------------- */

static void loop(sp_session *session) {
	int i = 0;
	int timeout = -1;
	sp_error error;
#ifdef _WIN32
	DWORD dwWaitStatus;
#else
	sigset_t sigset;

	sigemptyset(&sigset);
	sigaddset(&sigset, SIGIO);
#endif

	while (g_exit_code < 0) {

#ifndef _WIN32
		pthread_sigmask(SIG_BLOCK, &sigset, NULL);
#endif


		DSFYDEBUG("MAINLOOP: Calling sp_session_process_events()\n");
		sp_session_process_events(session, &timeout);
		DSFYDEBUG("MAINLOOP: Returned from sp_session_process_events(), sleeping %dms\n", timeout);


#ifdef _WIN32
		dwWaitStatus = WaitForSingleObject(notifyEvent, timeout);
		switch(dwWaitStatus) {
		case WAIT_ABANDONED:
			DSFYDEBUG("MAINLOOP: WaitForSingleObject() returned WAIT_ABANDONED\n");
			break;
		case WAIT_OBJECT_0:
			DSFYDEBUG("MAINLOOP: WaitForSingleObject() returned WAIT_OBJECT_0\n");
			break;
		case WAIT_TIMEOUT:
			DSFYDEBUG("MAINLOOP: WaitForSingleObject() returned WAIT_TIMEOUT\n");
			break;
		case WAIT_FAILED:
			DSFYDEBUG("MAINLOOP: WaitForSingleObject() returned WAIT_FAILED\n");
			break;
		default:
			DSFYDEBUG("MAINLOOP: WaitForSingleObject() returned unknown value 0x%08x\n", dwWaitStatus);
			break;
		}
#else
		pthread_sigmask(SIG_UNBLOCK, &sigset, NULL);
		usleep(timeout * 1000);
#endif
	

		if(++i == 15) {
			DSFYDEBUG("MAINLOOP: i==%d, calling sp_session_logout()\n", i);
			error = sp_session_logout(session);
			DSFYDEBUG("MAINLOOP: Returned from sp_session_logout(), error = %d\n", error);

			if (SP_ERROR_OK != error) {
				fprintf(stderr, "failed to log out from Spotify: %s\n",
					sp_error_message(error));
				g_exit_code = 5;
				return;
			}
		}
	}
}


#ifndef _WIN32
static void sigIgn(int signo) {
	DSFYDEBUG("SIGHANDLER: Interrupting sleeps with signal %d\n", signo);
}
#endif


int main(int argc, char **argv)
{
	sp_session_config config;
	sp_error error;
	sp_session *session;

	// Sending passwords on the command line is bad in general.
	// We do it here for brevity.
	if (argc < 3 || argv[1][0] == '-') {
		fprintf(stderr, "usage: %s <username> <password>\n", argv[0]);
		return 1;
	}


    // Setup for waking up the main thread in notify_main_thread()
    DSFYDEBUG("PING from main()\n");
#ifdef _WIN32
	notifyEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
#else
	g_main_thread = pthread_self();
	signal(SIGIO, &sigIgn);
#endif


    // Always do this. It allows libspotify to check for
    // header/library inconsistencies.
    config.api_version = SPOTIFY_API_VERSION;

    // The path of the directory to store the cache. This must be specified.
    // Please read the documentation on preferred values.
    config.cache_location = "tmp";

    // The path of the directory to store the settings. This must be specified.
    // Please read the documentation on preferred values.
    config.settings_location = "tmp";

    // The key of the application. They are generated by Spotify,
    // and are specific to each application using libspotify.
    config.application_key = g_appkey;
    config.application_key_size = g_appkey_size;

    // This identifies the application using some
    // free-text string [1, 255] characters.
    config.user_agent = "spotify-session-example";

    // Register the callbacks.
    config.callbacks = &g_callbacks;

    DSFYDEBUG("Initializing session with sp_session_init()\n");
    error = sp_session_init(&config, &session);

    if (SP_ERROR_OK != error) {
        fprintf(stderr, "failed to create session: %s\n",
                        sp_error_message(error));
        return 2;
    }

    // Login using the credentials given on the command line.
    DSFYDEBUG("Calling sp_session_login()\n");
    error = sp_session_login(session, argv[1], argv[2]);

    if (SP_ERROR_OK != error) {
        fprintf(stderr, "failed to login: %s\n",
                        sp_error_message(error));
        return 3;
    }

    DSFYDEBUG("Returned from sp_session_login()\n");

    loop(session);
    session_terminated();

    return 0;
}


void session_ready(sp_session *session) {
	DSFYDEBUG("Via login_callback, now in session_ready(session=%p)\n", session);
}


void session_terminated(void) {
	DSFYDEBUG("Session TERMINATED\n");
}
