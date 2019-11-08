/*
 * BlueALSA - sco.c
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "sco.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/sco.h>

#include "a2dp.h"
#include "ba-device.h"
#include "bluealsa.h"
#include "hci.h"
#include "hfp.h"
#include "msbc.h"
#include "utils.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

/**
 * SCO dispatcher internal data. */
struct sco_data {
	struct ba_adapter *a;
	struct pollfd pfd;
};

static void sco_dispatcher_cleanup(struct sco_data *data) {
	debug("SCO dispatcher cleanup: %s", data->a->hci.name);
	if (data->pfd.fd != -1)
		close(data->pfd.fd);
	data->a->sco_dispatcher = config.main_thread;
}

static void *sco_dispatcher_thread(struct ba_adapter *a) {

	struct sco_data data = { .a = a, .pfd = { -1, POLLIN, 0 } };

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(sco_dispatcher_cleanup), &data);

	if ((data.pfd.fd = hci_sco_open(data.a->hci.dev_id)) == -1) {
		error("Couldn't open SCO socket: %s", strerror(errno));
		goto fail;
	}

#if ENABLE_MSBC
	uint32_t defer = 1;
	if (setsockopt(data.pfd.fd, SOL_BLUETOOTH, BT_DEFER_SETUP, &defer, sizeof(defer)) == -1) {
		error("Couldn't set deferred connection setup: %s", strerror(errno));
		goto fail;
	}
#endif

	if (listen(data.pfd.fd, 10) == -1) {
		error("Couldn't listen on SCO socket: %s", strerror(errno));
		goto fail;
	}

	debug("Starting SCO dispatcher loop: %s", a->hci.name);
	for (;;) {

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		if (poll(&data.pfd, 1, -1) == -1) {
			if (errno == EINTR)
				continue;
			error("SCO dispatcher poll error: %s", strerror(errno));
			goto fail;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		struct sockaddr_sco addr;
		socklen_t addrlen = sizeof(addr);
		struct ba_device *d = NULL;
		struct ba_transport *t = NULL;
		char *ba_dbus_path = NULL;
		int fd = -1;

		if ((fd = accept(data.pfd.fd, (struct sockaddr *)&addr, &addrlen)) == -1) {
			error("Couldn't accept incoming SCO link: %s", strerror(errno));
			goto cleanup;
		}

		debug("New incoming SCO link: %s: %d", batostr_(&addr.sco_bdaddr), fd);

		if ((d = ba_device_lookup(data.a, &addr.sco_bdaddr)) == NULL) {
			error("Couldn't lookup device: %s", batostr_(&addr.sco_bdaddr));
			goto cleanup;
		}

		ba_dbus_path = g_strdup_printf("%s/sco", d->bluez_dbus_path);
		if ((t = ba_transport_lookup(d, ba_dbus_path)) == NULL) {
			error("Couldn't lookup transport: %s", ba_dbus_path);
			goto cleanup;
		}

#if ENABLE_MSBC
		struct bt_voice voice = { .setting = BT_VOICE_TRANSPARENT };
		if (t->type.codec == HFP_CODEC_MSBC &&
				setsockopt(fd, SOL_BLUETOOTH, BT_VOICE, &voice, sizeof(voice)) == -1) {
			error("Couldn't setup transparent voice: %s", strerror(errno));
			goto cleanup;
		}
		if (read(fd, &voice, 1) == -1) {
			error("Couldn't authorize SCO connection: %s", strerror(errno));
			goto cleanup;
		}
#endif

		t->bt_fd = fd;
		t->mtu_read = t->mtu_write = hci_sco_get_mtu(fd);
		fd = -1;

		ba_transport_send_signal(t, TRANSPORT_PING);

cleanup:
		if (d != NULL)
			ba_device_unref(d);
		if (t != NULL)
			ba_transport_unref(t);
		if (ba_dbus_path != NULL)
			g_free(ba_dbus_path);
		if (fd != -1)
			close(fd);

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(1);
	return NULL;
}

int sco_setup_connection_dispatcher(struct ba_adapter *a) {

	/* skip setup if dispatcher thread is already running */
	if (!pthread_equal(a->sco_dispatcher, config.main_thread))
		return 0;

	int ret;

	/* Please note, that during the SCO dispatcher thread creation the adapter
	 * is not referenced. It is guaranteed that the adapter will be available
	 * during the whole live-span of the thread, because the thread is canceled
	 * in the adapter cleanup routine. See the ba_adapter_unref() function. */
	if ((ret = pthread_create(&a->sco_dispatcher, NULL,
					PTHREAD_ROUTINE(sco_dispatcher_thread), a)) != 0) {
		error("Couldn't create SCO dispatcher: %s", strerror(ret));
		a->sco_dispatcher = config.main_thread;
		return -1;
	}

	pthread_setname_np(a->sco_dispatcher, "ba-sco-dispatch");
	debug("Created SCO dispatcher [%s]: %s", "ba-sco-dispatch", a->hci.name);

	return 0;
}

void *sco_thread(struct ba_transport *t) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup), t);

	/* buffers for transferring data to and from SCO socket */
	ffb_uint8_t bt_in = { 0 };
	ffb_uint8_t bt_out = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_uint8_free), &bt_in);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_uint8_free), &bt_out);

#if ENABLE_MSBC
	struct esco_msbc msbc = { .init = false };
	pthread_cleanup_push(PTHREAD_CLEANUP(msbc_finish), &msbc);
#endif

	/* these buffers shall be bigger than the SCO MTU */
	if (ffb_init(&bt_in, 128) == NULL ||
			ffb_init(&bt_out, 128) == NULL) {
		error("Couldn't create data buffer: %s", strerror(ENOMEM));
		goto fail_ffb;
	}

	int poll_timeout = -1;
	struct asrsync asrs = { .frames = 0 };
	struct pollfd pfds[] = {
		{ t->sig_fd[0], POLLIN, 0 },
		/* SCO socket */
		{ -1, POLLIN, 0 },
		{ -1, POLLOUT, 0 },
		/* PCM FIFO */
		{ -1, POLLIN, 0 },
		{ -1, POLLOUT, 0 },
	};

	debug("Starting SCO loop: %s", ba_transport_type_to_string(t->type));
	for (;;) {
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		/* fresh-start for file descriptors polling */
		pfds[1].fd = pfds[2].fd = -1;
		pfds[3].fd = pfds[4].fd = -1;

		switch (t->type.codec) {
#if ENABLE_MSBC
		case HFP_CODEC_MSBC:
			msbc_encode(&msbc);
			msbc_decode(&msbc);
			if (t->mtu_read > 0 && ffb_blen_in(&msbc.dec_data) >= t->mtu_read)
				pfds[1].fd = t->bt_fd;
			if (t->mtu_write > 0 && ffb_blen_out(&msbc.enc_data) >= t->mtu_write)
				pfds[2].fd = t->bt_fd;
			if (t->mtu_write > 0 && ffb_blen_in(&msbc.enc_pcm) >= t->mtu_write)
				pfds[3].fd = t->sco.spk_pcm.fd;
			if (ffb_blen_out(&msbc.dec_pcm) > 0)
				pfds[4].fd = t->sco.mic_pcm.fd;
			break;
#endif
		case HFP_CODEC_CVSD:
		default:
			if (t->mtu_read > 0 && ffb_len_in(&bt_in) >= t->mtu_read)
				pfds[1].fd = t->bt_fd;
			if (t->mtu_write > 0 && ffb_len_out(&bt_out) >= t->mtu_write)
				pfds[2].fd = t->bt_fd;
			if (t->mtu_write > 0 && ffb_len_in(&bt_out) >= t->mtu_write)
				pfds[3].fd = t->sco.spk_pcm.fd;
			if (ffb_len_out(&bt_in) > 0)
				pfds[4].fd = t->sco.mic_pcm.fd;
		}

		/* In order not to run this this loop unnecessarily, do not poll SCO for
		 * reading if microphone (capture) PCM is not connected. For oFono this
		 * rule does not apply, because we will use read error for SCO release. */
		if (!t->sco.ofono && t->sco.mic_pcm.fd == -1)
			pfds[1].fd = -1;

		switch (poll(pfds, ARRAYSIZE(pfds), poll_timeout)) {
		case 0:
			pthread_cond_signal(&t->sco.spk_drained);
			poll_timeout = -1;
			continue;
		case -1:
			if (errno == EINTR)
				continue;
			error("Transport poll error: %s", strerror(errno));
			goto fail;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		if (pfds[0].revents & POLLIN) {
			/* dispatch incoming event */

			switch (ba_transport_recv_signal(t)) {
			case TRANSPORT_PING:
			case TRANSPORT_PCM_OPEN:
			case TRANSPORT_PCM_RESUME:
				poll_timeout = -1;
				asrs.frames = 0;
				break;
			case TRANSPORT_PCM_SYNC:
				/* FIXME: Drain functionality for speaker.
				 * XXX: Right now it is not possible to drain speaker PCM (in a clean
				 *      fashion), because poll() will not timeout if we've got incoming
				 *      data from the microphone (BT SCO socket). In order not to hang
				 *      forever in the transport_drain_pcm() function, we will signal
				 *      PCM drain right now. */
				pthread_cond_signal(&t->sco.spk_drained);
				break;
			case TRANSPORT_PCM_DROP:
				io_thread_read_pcm_flush(&t->sco.spk_pcm);
				continue;
			default:
				break;
			}

			/* connection is managed by oFono */
			if (t->sco.ofono)
				continue;

			const enum hfp_ind *inds = t->sco.rfcomm->rfcomm.hfp_inds;
			bool release = false;

			/* It is required to release SCO if we are not transferring audio,
			 * because it will free Bluetooth bandwidth - microphone signal is
			 * transfered even though we are not reading from it! */
			if (t->sco.spk_pcm.fd == -1 && t->sco.mic_pcm.fd == -1)
				release = true;

			/* For HFP HF we have to check if we are in the call stage or in the
			 * call setup stage. Otherwise, it might be not possible to acquire
			 * SCO connection. */
			if (t->type.profile == BA_TRANSPORT_PROFILE_HFP_HF &&
					inds[HFP_IND_CALL] == HFP_IND_CALL_NONE &&
					inds[HFP_IND_CALLSETUP] == HFP_IND_CALLSETUP_NONE)
				release = true;

			if (release) {
				t->release(t);
				asrs.frames = 0;
			}
			else {
				t->acquire(t);
#if ENABLE_MSBC
				/* this can be called again, make sure it is idempotent */
				if (t->type.codec == HFP_CODEC_MSBC && msbc_init(&msbc) != 0) {
					error("Couldn't initialize mSBC codec: %s", strerror(errno));
					goto fail;
				}
#endif
			}

			continue;
		}

		if (asrs.frames == 0)
			asrsync_init(&asrs, ba_transport_get_sampling(t));

		if (pfds[1].revents & POLLIN) {
			/* dispatch incoming SCO data */

			uint8_t *buffer;
			size_t buffer_len;
			ssize_t len;

			switch (t->type.codec) {
#if ENABLE_MSBC
			case HFP_CODEC_MSBC:
				buffer = msbc.dec_data.tail;
				buffer_len = ffb_len_in(&msbc.dec_data);
				break;
#endif
			case HFP_CODEC_CVSD:
			default:
				if (t->sco.mic_pcm.fd == -1)
					ffb_rewind(&bt_in);
				buffer = bt_in.tail;
				buffer_len = ffb_len_in(&bt_in);
			}

retry_sco_read:
			errno = 0;
			if ((len = read(pfds[1].fd, buffer, buffer_len)) <= 0)
				switch (errno) {
				case EINTR:
					goto retry_sco_read;
				case 0:
				case ECONNABORTED:
				case ECONNRESET:
					t->release(t);
					continue;
				default:
					error("SCO read error: %s", strerror(errno));
					continue;
				}

			switch (t->type.codec) {
#if ENABLE_MSBC
			case HFP_CODEC_MSBC:
				ffb_seek(&msbc.dec_data, len);
				break;
#endif
			case HFP_CODEC_CVSD:
			default:
				ffb_seek(&bt_in, len);
			}

		}
		else if (pfds[1].revents & (POLLERR | POLLHUP)) {
			debug("SCO poll error status: %#x", pfds[1].revents);
			t->release(t);
		}

		if (pfds[2].revents & POLLOUT) {
			/* write-out SCO data */

			uint8_t *buffer;
			size_t buffer_len;
			ssize_t len;

			switch (t->type.codec) {
#if ENABLE_MSBC
			case HFP_CODEC_MSBC:
				buffer = msbc.enc_data.data;
				buffer_len = t->mtu_write;
				break;
#endif
			case HFP_CODEC_CVSD:
			default:
				buffer = bt_out.data;
				buffer_len = t->mtu_write;
			}

retry_sco_write:
			errno = 0;
			if ((len = write(pfds[2].fd, buffer, buffer_len)) <= 0)
				switch (errno) {
				case EINTR:
					goto retry_sco_write;
				case 0:
				case ECONNABORTED:
				case ECONNRESET:
					t->release(t);
					continue;
				default:
					error("SCO write error: %s", strerror(errno));
					continue;
				}

			switch (t->type.codec) {
#if ENABLE_MSBC
			case HFP_CODEC_MSBC:
				ffb_shift(&msbc.enc_data, len);
				break;
#endif
			case HFP_CODEC_CVSD:
			default:
				ffb_shift(&bt_out, len);
			}

		}

		if (pfds[3].revents & POLLIN) {
			/* dispatch incoming PCM data */

			int16_t *buffer;
			ssize_t samples;

			switch (t->type.codec) {
#if ENABLE_MSBC
			case HFP_CODEC_MSBC:
				buffer = msbc.enc_pcm.tail;
				samples = ffb_len_in(&msbc.enc_pcm);
				break;
#endif
			case HFP_CODEC_CVSD:
			default:
				buffer = (int16_t *)bt_out.tail;
				samples = ffb_len_in(&bt_out) / sizeof(int16_t);
			}

			if ((samples = io_thread_read_pcm(&t->sco.spk_pcm, buffer, samples)) <= 0) {
				if (samples == -1 && errno != EAGAIN)
					error("PCM read error: %s", strerror(errno));
				if (samples == 0)
					ba_transport_send_signal(t, TRANSPORT_PCM_CLOSE);
				continue;
			}

			if (t->sco.spk_muted)
				snd_pcm_scale_s16le(buffer, samples, 1, 0, 0);

			switch (t->type.codec) {
#if ENABLE_MSBC
			case HFP_CODEC_MSBC:
				ffb_seek(&msbc.enc_pcm, samples);
				break;
#endif
			case HFP_CODEC_CVSD:
			default:
				ffb_seek(&bt_out, samples * sizeof(int16_t));
			}

		}
		else if (pfds[3].revents & (POLLERR | POLLHUP)) {
			debug("PCM poll error status: %#x", pfds[3].revents);
			ba_transport_release_pcm(&t->sco.spk_pcm);
			ba_transport_send_signal(t, TRANSPORT_PCM_CLOSE);
		}

		if (pfds[4].revents & POLLOUT) {
			/* write-out PCM data */

			int16_t *buffer;
			ssize_t samples;

			switch (t->type.codec) {
#if ENABLE_MSBC
			case HFP_CODEC_MSBC:
				buffer = msbc.dec_pcm.data;
				samples = ffb_len_out(&msbc.dec_pcm);
				break;
#endif
			case HFP_CODEC_CVSD:
			default:
				buffer = (int16_t *)bt_in.data;
				samples = ffb_len_out(&bt_in) / sizeof(int16_t);
			}

			if (t->sco.mic_muted)
				snd_pcm_scale_s16le(buffer, samples, 1, 0, 0);

			if ((samples = io_thread_write_pcm(&t->sco.mic_pcm, buffer, samples)) <= 0) {
				if (samples == -1)
					error("FIFO write error: %s", strerror(errno));
				if (samples == 0)
					ba_transport_send_signal(t, TRANSPORT_PCM_CLOSE);
			}

			switch (t->type.codec) {
#if ENABLE_MSBC
			case HFP_CODEC_MSBC:
				ffb_shift(&msbc.dec_pcm, samples);
				break;
#endif
			case HFP_CODEC_CVSD:
			default:
				ffb_shift(&bt_in, samples * sizeof(int16_t));
			}

		}

		/* keep data transfer at a constant bit rate */
		asrsync_sync(&asrs, t->mtu_write / 2);
		/* update busy delay (encoding overhead) */
		t->delay = asrsync_get_busy_usec(&asrs) / 100;

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
fail_ffb:
#if ENABLE_MSBC
	pthread_cleanup_pop(1);
#endif
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}