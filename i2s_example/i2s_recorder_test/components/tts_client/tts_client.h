#ifndef TTS_CLIENT_H
#define TTS_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Sends a text string to Google Translate TTS, streams the MP3 response,
 * decodes it on the fly using minimp3, and plays it via audio_hal.
 *
 * @param text The text to convert to speech.
 */
void tts_client_play(const char *text);

#ifdef __cplusplus
}
#endif

#endif // TTS_CLIENT_H
