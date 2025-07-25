/*
 * PgBouncer - Lightweight connection pooler for PostgreSQL.
 *
 * Copyright (c) 2007-2009  Marko Kreen, Skype Technologies OÜ
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * SCRAM support
 */

#include "bouncer.h"
#include "scram.h"
#include "common/base64.h"
#include "common/saslprep.h"
#include "common/scram-common.h"


static bool calculate_client_proof(ScramState *scram_state,
				   const PgCredentials *credentials,
				   const char *salt,
				   int saltlen,
				   int iterations,
				   const char *client_final_message_without_proof,
				   uint8_t *result);


/*
 * free SCRAM state info after auth is done
 */
void free_scram_state(ScramState *scram_state)
{
	free(scram_state->client_nonce);
	free(scram_state->client_first_message_bare);
	free(scram_state->client_final_message_without_proof);
	free(scram_state->server_nonce);
	free(scram_state->server_first_message);
	free(scram_state->SaltedPassword);
	free(scram_state->salt);
	memset(scram_state, 0, sizeof(*scram_state));
}

static bool is_scram_printable(char *p)
{
	/*------
	 * Printable characters, as defined by SCRAM spec: (RFC 5802)
	 *
	 *  printable       = %x21-2B / %x2D-7E
	 *                    ;; Printable ASCII except ",".
	 *                    ;; Note that any "printable" is also
	 *                    ;; a valid "value".
	 *------
	 */
	for (; *p; p++)
		if (*p < 0x21 || *p > 0x7E || *p == 0x2C /* comma */)
			return false;

	return true;
}

static char *sanitize_char(char c)
{
	static char buf[5];

	if (c >= 0x21 && c <= 0x7E)
		snprintf(buf, sizeof(buf), "'%c'", c);
	else
		snprintf(buf, sizeof(buf), "0x%02x", (unsigned char) c);
	return buf;
}

/*
 * Read value for an attribute part of a SCRAM message.
 */
static char *read_attr_value(PgSocket *sk, char **input, char attr)
{
	char *begin = *input;
	char *end;

	if (*begin != attr) {
		slog_error(sk, "malformed SCRAM message (attribute \"%c\" expected)",
			   attr);
		return NULL;
	}
	begin++;

	if (*begin != '=') {
		slog_error(sk, "malformed SCRAM message (expected \"=\" after attribute \"%c\")",
			   attr);
		return NULL;
	}
	begin++;

	end = begin;
	while (*end && *end != ',')
		end++;

	if (*end) {
		*end = '\0';
		*input = end + 1;
	} else {
		*input = end;
	}

	return begin;
}

/*
 * Read the next attribute and value in a SCRAM exchange message.
 *
 * Returns NULL if there is no attribute.
 */
static char *read_any_attr(PgSocket *sk, char **input, char *attr_p)
{
	char *begin = *input;
	char *end;
	char attr = *begin;

	if (!((attr >= 'A' && attr <= 'Z') ||
	      (attr >= 'a' && attr <= 'z'))) {
		slog_error(sk, "malformed SCRAM message (attribute expected, but found invalid character \"%s\")",
			   sanitize_char(attr));
		return NULL;
	}
	if (attr_p)
		*attr_p = attr;
	begin++;

	if (*begin != '=') {
		slog_error(sk, "malformed SCRAM message (expected character \"=\" after attribute \"%c\")",
			   attr);
		return NULL;
	}
	begin++;

	end = begin;
	while (*end && *end != ',')
		end++;

	if (*end) {
		*end = '\0';
		*input = end + 1;
	} else {
		*input = end;
	}

	return begin;
}

/*
 * Parse and validate format of given SCRAM secret.
 *
 * Returns true if the SCRAM secret has been parsed, and false otherwise.
 */
static bool parse_scram_secret(const char *secret, int *iterations, char **salt,
			       uint8_t *stored_key, uint8_t *server_key)
{
	char *s;
	char *p;
	char *scheme_str;
	char *salt_str;
	char *iterations_str;
	char *storedkey_str;
	char *serverkey_str;
	int decoded_len;
	char *decoded_salt_buf;
	char *decoded_stored_buf = NULL;
	char *decoded_server_buf = NULL;

	/*
	 * The secret is of form:
	 *
	 * SCRAM-SHA-256$<iterations>:<salt>$<storedkey>:<serverkey>
	 */
	s = strdup(secret);
	if (!s)
		goto invalid_secret;
	if ((scheme_str = strtok(s, "$")) == NULL)
		goto invalid_secret;
	if ((iterations_str = strtok(NULL, ":")) == NULL)
		goto invalid_secret;
	if ((salt_str = strtok(NULL, "$")) == NULL)
		goto invalid_secret;
	if ((storedkey_str = strtok(NULL, ":")) == NULL)
		goto invalid_secret;
	if ((serverkey_str = strtok(NULL, "")) == NULL)
		goto invalid_secret;

	/* Parse the fields */
	if (strcmp(scheme_str, "SCRAM-SHA-256") != 0)
		goto invalid_secret;

	errno = 0;
	*iterations = strtol(iterations_str, &p, 10);
	if (*p || errno != 0)
		goto invalid_secret;

	/*
	 * Verify that the salt is in Base64-encoded format, by decoding it,
	 * although we return the encoded version to the caller.
	 */
	decoded_len = pg_b64_dec_len(strlen(salt_str));
	decoded_salt_buf = malloc(decoded_len);
	if (!decoded_salt_buf)
		goto invalid_secret;
	decoded_len = pg_b64_decode(salt_str, strlen(salt_str), decoded_salt_buf, decoded_len);
	free(decoded_salt_buf);
	if (decoded_len < 0)
		goto invalid_secret;
	*salt = strdup(salt_str);
	if (!*salt)
		goto invalid_secret;

	/*
	 * Decode StoredKey and ServerKey.
	 */
	decoded_len = pg_b64_dec_len(strlen(storedkey_str));
	decoded_stored_buf = malloc(decoded_len);
	if (!decoded_stored_buf)
		goto invalid_secret;
	decoded_len = pg_b64_decode(storedkey_str, strlen(storedkey_str), decoded_stored_buf, decoded_len);
	if (decoded_len != SCRAM_KEY_LEN)
		goto invalid_secret;
	memcpy(stored_key, decoded_stored_buf, SCRAM_KEY_LEN);

	decoded_len = pg_b64_dec_len(strlen(serverkey_str));
	decoded_server_buf = malloc(decoded_len);
	decoded_len = pg_b64_decode(serverkey_str, strlen(serverkey_str),
				    decoded_server_buf, decoded_len);
	if (decoded_len != SCRAM_KEY_LEN)
		goto invalid_secret;
	memcpy(server_key, decoded_server_buf, SCRAM_KEY_LEN);

	free(decoded_stored_buf);
	free(decoded_server_buf);
	free(s);
	return true;

invalid_secret:
	free(decoded_stored_buf);
	free(decoded_server_buf);
	free(s);
	free(*salt);
	*salt = NULL;
	return false;
}

#define MD5_PASSWD_CHARSET "0123456789abcdef"

/*
 * What kind of a password type is 'shadow_pass'?
 */
PasswordType get_password_type(const char *shadow_pass)
{
	char *encoded_salt = NULL;
	int iterations;
	uint8_t stored_key[SCRAM_KEY_LEN];
	uint8_t server_key[SCRAM_KEY_LEN];

	if (strncmp(shadow_pass, "md5", 3) == 0 &&
	    strlen(shadow_pass) == MD5_PASSWD_LEN &&
	    strspn(shadow_pass + 3, MD5_PASSWD_CHARSET) == MD5_PASSWD_LEN - 3)
		return PASSWORD_TYPE_MD5;
	if (parse_scram_secret(shadow_pass, &iterations, &encoded_salt,
			       stored_key, server_key)) {
		free(encoded_salt);
		return PASSWORD_TYPE_SCRAM_SHA_256;
	}
	free(encoded_salt);
	return PASSWORD_TYPE_PLAINTEXT;
}

/*
 * Functions for communicating as a client with the server
 */

char *build_client_first_message(ScramState *scram_state)
{
	uint8_t raw_nonce[SCRAM_RAW_NONCE_LEN + 1];
	int encoded_len;
	size_t len;
	char *result = NULL;

	get_random_bytes(raw_nonce, SCRAM_RAW_NONCE_LEN);

	encoded_len = pg_b64_enc_len(SCRAM_RAW_NONCE_LEN);
	scram_state->client_nonce = malloc(encoded_len + 1);
	if (scram_state->client_nonce == NULL)
		goto failed;
	encoded_len = pg_b64_encode((char *) raw_nonce, SCRAM_RAW_NONCE_LEN, scram_state->client_nonce, encoded_len);
	if (encoded_len < 0)
		goto failed;
	scram_state->client_nonce[encoded_len] = '\0';

	len = 8 + strlen(scram_state->client_nonce) + 1;
	result = malloc(len);
	if (result == NULL)
		goto failed;
	snprintf(result, len, "n,,n=,r=%s", scram_state->client_nonce);

	scram_state->client_first_message_bare = strdup(result + 3);
	if (scram_state->client_first_message_bare == NULL)
		goto failed;

	return result;

failed:
	free(result);
	free(scram_state->client_nonce);
	free(scram_state->client_first_message_bare);
	return NULL;
}

char *build_client_final_message(ScramState *scram_state,
				 const PgCredentials *credentials,
				 const char *server_nonce,
				 const char *salt,
				 int saltlen,
				 int iterations)
{
	char buf[512];
	size_t len;
	uint8_t client_proof[SCRAM_KEY_LEN];
	int enclen;

	snprintf(buf, sizeof(buf), "c=biws,r=%s", server_nonce);

	scram_state->client_final_message_without_proof = strdup(buf);
	if (scram_state->client_final_message_without_proof == NULL)
		goto failed;

	if (!calculate_client_proof(scram_state, credentials,
				    salt, saltlen, iterations, buf,
				    client_proof))
		goto failed;

	len = strlcat(buf, ",p=", sizeof(buf));
	enclen = pg_b64_enc_len(sizeof(client_proof));
	enclen = pg_b64_encode((char *) client_proof,
			       SCRAM_KEY_LEN,
			       buf + len, enclen);
	if (enclen < 0)
		goto failed;
	len += enclen;
	buf[len] = '\0';

	return strdup(buf);
failed:
	return NULL;
}

bool read_server_first_message(PgSocket *server, char *input,
			       char **server_nonce_p, char **salt_p, int *saltlen_p, int *iterations_p)
{
	char *server_nonce;
	char *encoded_salt;
	char *salt = NULL;
	int saltlen;
	char *iterations_str;
	char *endptr;
	int iterations;

	server->scram_state.server_first_message = strdup(input);
	if (server->scram_state.server_first_message == NULL)
		goto failed;

	server_nonce = read_attr_value(server, &input, 'r');
	if (server_nonce == NULL)
		goto failed;

	if (strlen(server_nonce) < strlen(server->scram_state.client_nonce) ||
	    memcmp(server_nonce, server->scram_state.client_nonce, strlen(server->scram_state.client_nonce)) != 0) {
		slog_error(server, "invalid SCRAM response (nonce mismatch)");
		goto failed;
	}

	encoded_salt = read_attr_value(server, &input, 's');
	if (encoded_salt == NULL)
		goto failed;
	saltlen = pg_b64_dec_len(strlen(encoded_salt));
	salt = malloc(saltlen);
	if (salt == NULL)
		goto failed;
	saltlen = pg_b64_decode(encoded_salt,
				strlen(encoded_salt),
				salt, saltlen);
	if (saltlen < 0) {
		slog_error(server, "malformed SCRAM message (invalid salt)");
		goto failed;
	}

	iterations_str = read_attr_value(server, &input, 'i');
	if (iterations_str == NULL)
		goto failed;

	iterations = strtol(iterations_str, &endptr, 10);
	if (*endptr != '\0' || iterations < 1) {
		slog_error(server, "malformed SCRAM message (invalid iteration count)");
		goto failed;
	}

	if (*input != '\0') {
		slog_error(server, "malformed SCRAM message (garbage at end of server-first-message)");
		goto failed;
	}

	*server_nonce_p = server_nonce;
	*salt_p = salt;
	*saltlen_p = saltlen;
	*iterations_p = iterations;
	return true;
failed:
	free(salt);
	return false;
}

bool read_server_final_message(PgSocket *server, char *input, char *ServerSignature)
{
	char *encoded_server_signature;
	char *decoded_server_signature = NULL;
	int server_signature_len;

	if (*input == 'e') {
		char *errmsg = read_attr_value(server, &input, 'e');
		slog_error(server, "error received from server in SCRAM exchange: %s",
			   errmsg);
		goto failed;
	}

	encoded_server_signature = read_attr_value(server, &input, 'v');
	if (encoded_server_signature == NULL)
		goto failed;

	if (*input != '\0')
		slog_error(server, "malformed SCRAM message (garbage at end of server-final-message)");

	server_signature_len = pg_b64_dec_len(strlen(encoded_server_signature));
	decoded_server_signature = malloc(server_signature_len);
	if (!decoded_server_signature)
		goto failed;

	server_signature_len = pg_b64_decode(encoded_server_signature,
					     strlen(encoded_server_signature),
					     decoded_server_signature,
					     server_signature_len);
	if (server_signature_len != SCRAM_KEY_LEN) {
		slog_error(server, "malformed SCRAM message (malformed server signature)");
		goto failed;
	}
	memcpy(ServerSignature, decoded_server_signature, SCRAM_KEY_LEN);

	free(decoded_server_signature);
	return true;
failed:
	free(decoded_server_signature);
	return false;
}

static bool calculate_client_proof(ScramState *scram_state,
				   const PgCredentials *credentials,
				   const char *salt,
				   int saltlen,
				   int iterations,
				   const char *client_final_message_without_proof,
				   uint8_t *result)
{
	pg_saslprep_rc rc;
	char *prep_password = NULL;
	uint8_t StoredKey[SCRAM_KEY_LEN];
	uint8_t ClientKey[SCRAM_KEY_LEN];
	uint8_t ClientSignature[SCRAM_KEY_LEN];
	scram_HMAC_ctx ctx;

	if (credentials->use_scram_keys) {
		memcpy(ClientKey, credentials->scram_ClientKey, SCRAM_KEY_LEN);
	} else
	{
		rc = pg_saslprep(credentials->passwd, &prep_password);
		if (rc == SASLPREP_OOM)
			return false;
		if (rc != SASLPREP_SUCCESS) {
			prep_password = strdup(credentials->passwd);
			if (!prep_password)
				return false;
		}

		scram_state->SaltedPassword = malloc(SCRAM_KEY_LEN);
		if (scram_state->SaltedPassword == NULL)
			goto failed;
		scram_SaltedPassword(prep_password,
				     salt,
				     saltlen,
				     iterations,
				     scram_state->SaltedPassword);

		scram_ClientKey(scram_state->SaltedPassword, ClientKey);
	}

	scram_H(ClientKey, SCRAM_KEY_LEN, StoredKey);

	scram_HMAC_init(&ctx, StoredKey, SCRAM_KEY_LEN);
	scram_HMAC_update(&ctx,
			  scram_state->client_first_message_bare,
			  strlen(scram_state->client_first_message_bare));
	scram_HMAC_update(&ctx, ",", 1);
	scram_HMAC_update(&ctx,
			  scram_state->server_first_message,
			  strlen(scram_state->server_first_message));
	scram_HMAC_update(&ctx, ",", 1);
	scram_HMAC_update(&ctx,
			  client_final_message_without_proof,
			  strlen(client_final_message_without_proof));
	scram_HMAC_final(ClientSignature, &ctx);

	for (int i = 0; i < SCRAM_KEY_LEN; i++)
		result[i] = ClientKey[i] ^ ClientSignature[i];

	free(prep_password);
	return true;
failed:
	free(prep_password);
	return false;
}

bool verify_server_signature(ScramState *scram_state, const PgCredentials *credentials, const char *ServerSignature)
{
	uint8_t expected_ServerSignature[SCRAM_KEY_LEN];
	uint8_t ServerKey[SCRAM_KEY_LEN];
	scram_HMAC_ctx ctx;

	if (credentials->use_scram_keys)
		memcpy(ServerKey, credentials->scram_ServerKey, SCRAM_KEY_LEN);
	else
		scram_ServerKey(scram_state->SaltedPassword, ServerKey);

	scram_HMAC_init(&ctx, ServerKey, SCRAM_KEY_LEN);
	scram_HMAC_update(&ctx,
			  scram_state->client_first_message_bare,
			  strlen(scram_state->client_first_message_bare));
	scram_HMAC_update(&ctx, ",", 1);
	scram_HMAC_update(&ctx,
			  scram_state->server_first_message,
			  strlen(scram_state->server_first_message));
	scram_HMAC_update(&ctx, ",", 1);
	scram_HMAC_update(&ctx,
			  scram_state->client_final_message_without_proof,
			  strlen(scram_state->client_final_message_without_proof));
	scram_HMAC_final(expected_ServerSignature, &ctx);

	if (memcmp(expected_ServerSignature, ServerSignature, SCRAM_KEY_LEN) != 0)
		return false;

	return true;
}


/*
 * Functions for communicating as a server to the client
 */

bool read_client_first_message(PgSocket *client, char *input,
			       char *cbind_flag_p,
			       char **client_first_message_bare_p,
			       char **client_nonce_p)
{
	char *client_first_message_bare = NULL;
	char *client_nonce = NULL;
	char *client_nonce_copy = NULL;

	*cbind_flag_p = *input;
	switch (*input) {
	case 'n':
		/* Client does not support channel binding */
		input++;
		break;
	case 'y':
		/* Client supports channel binding, but we're not doing it today */
		input++;
		break;
	case 'p':
		/* Client requires channel binding.  We don't support it. */
		slog_error(client, "client requires SCRAM channel binding, but it is not supported");
		goto failed;
	default:
		slog_error(client, "malformed SCRAM message (unexpected channel-binding flag \"%s\")",
			   sanitize_char(*input));
		goto failed;
	}

	if (*input != ',') {
		slog_error(client, "malformed SCRAM message (comma expected, but found character \"%s\")",
			   sanitize_char(*input));
		goto failed;
	}
	input++;

	if (*input == 'a') {
		slog_error(client, "client uses authorization identity, but it is not supported");
		goto failed;
	}
	if (*input != ',') {
		slog_error(client, "malformed SCRAM message (unexpected attribute \"%s\" in client-first-message)",
			   sanitize_char(*input));
		goto failed;
	}
	input++;

	client_first_message_bare = strdup(input);
	if (client_first_message_bare == NULL)
		goto failed;

	if (*input == 'm') {
		slog_error(client, "client requires an unsupported SCRAM extension");
		goto failed;
	}

	/* read and ignore user name */
	read_attr_value(client, &input, 'n');

	client_nonce = read_attr_value(client, &input, 'r');
	if (client_nonce == NULL)
		goto failed;
	if (!is_scram_printable(client_nonce)) {
		slog_error(client, "non-printable characters in SCRAM nonce");
		goto failed;
	}
	client_nonce_copy = strdup(client_nonce);
	if (client_nonce_copy == NULL)
		goto failed;

	/*
	 * There can be any number of optional extensions after this.  We don't
	 * support any extensions, so ignore them.
	 */
	while (*input != '\0') {
		if (!read_any_attr(client, &input, NULL))
			goto failed;
	}

	*client_first_message_bare_p = client_first_message_bare;
	*client_nonce_p = client_nonce_copy;
	return true;
failed:
	free(client_first_message_bare);
	free(client_nonce_copy);
	return false;
}

bool read_client_final_message(PgSocket *client, const uint8_t *raw_input, char *input,
			       const char **client_final_nonce_p,
			       char **proof_p)
{
	const char *input_start = input;
	char attr;
	char *channel_binding;
	char *client_final_nonce;
	char *proof_start;
	char *value;
	char *encoded_proof;
	char *proof = NULL;
	int prooflen;

	/*
	 * Read channel-binding.  We don't support channel binding, so
	 * it's expected to always be "biws", which is "n,,",
	 * base64-encoded, or "eSws", which is "y,,".  We also have to
	 * check whether the flag is the same one that the client
	 * originally sent.
	 */
	channel_binding = read_attr_value(client, &input, 'c');
	if (channel_binding == NULL)
		goto failed;
	if (!(strcmp(channel_binding, "biws") == 0 && client->scram_state.cbind_flag == 'n') &&
	    !(strcmp(channel_binding, "eSws") == 0 && client->scram_state.cbind_flag == 'y')) {
		slog_error(client, "unexpected SCRAM channel-binding attribute in client-final-message");
		goto failed;
	}

	client_final_nonce = read_attr_value(client, &input, 'r');

	/* ignore optional extensions */
	do {
		proof_start = input - 1;
		value = read_any_attr(client, &input, &attr);
	} while (value && attr != 'p');

	if (!value) {
		slog_error(client, "could not read proof");
		goto failed;
	}

	encoded_proof = value;

	prooflen = pg_b64_dec_len(strlen(encoded_proof));
	proof = malloc(prooflen);
	if (proof == NULL) {
		slog_error(client, "could not decode proof");
		goto failed;
	}
	prooflen = pg_b64_decode(encoded_proof,
				 strlen(encoded_proof),
				 proof, prooflen);
	if (prooflen != SCRAM_KEY_LEN) {
		slog_error(client, "malformed SCRAM message (malformed proof in client-final-message)");
		goto failed;
	}

	if (*input != '\0') {
		slog_error(client, "malformed SCRAM message (garbage at the end of client-final-message)");
		goto failed;
	}

	client->scram_state.client_final_message_without_proof = malloc(proof_start - input_start + 1);
	if (!client->scram_state.client_final_message_without_proof)
		goto failed;
	memcpy(client->scram_state.client_final_message_without_proof, raw_input, proof_start - input_start);
	client->scram_state.client_final_message_without_proof[proof_start - input_start] = '\0';

	*client_final_nonce_p = client_final_nonce;
	*proof_p = proof;
	return true;
failed:
	free(proof);
	return false;
}

/*
 * For doing SCRAM with a password stored in plain text, build a SCRAM
 * secret on the fly.
 */
static bool build_adhoc_scram_secret(const char *plain_password, ScramState *scram_state)
{
	const char *password;
	char *prep_password;
	pg_saslprep_rc rc;
	char saltbuf[SCRAM_DEFAULT_SALT_LEN];
	int encoded_len;
	uint8_t salted_password[SCRAM_KEY_LEN];

	rc = pg_saslprep(plain_password, &prep_password);
	if (rc == SASLPREP_OOM)
		goto failed;
	else if (rc == SASLPREP_SUCCESS)
		password = prep_password;
	else
		password = plain_password;

	get_random_bytes((uint8_t *) saltbuf, sizeof(saltbuf));

	scram_state->adhoc = true;

	scram_state->iterations = cf_scram_iterations;

	encoded_len = pg_b64_enc_len(sizeof(saltbuf));
	scram_state->salt = malloc(encoded_len + 1);
	if (!scram_state->salt)
		goto failed;
	encoded_len = pg_b64_encode(saltbuf, sizeof(saltbuf), scram_state->salt, encoded_len);
	if (encoded_len < 0)
		goto failed;
	scram_state->salt[encoded_len] = '\0';

	/* Calculate StoredKey and ServerKey */
	scram_SaltedPassword(password, saltbuf, sizeof(saltbuf),
			     scram_state->iterations,
			     salted_password);
	scram_ClientKey(salted_password, scram_state->StoredKey);
	scram_H(scram_state->StoredKey, SCRAM_KEY_LEN, scram_state->StoredKey);
	scram_ServerKey(salted_password, scram_state->ServerKey);

	free(prep_password);
	return true;
failed:
	free(prep_password);
	return false;
}

/*
 * Deterministically generate salt for mock authentication, using a
 * SHA256 hash based on the username and an instance-level secret key.
 * Target buffer needs to be of size SCRAM_DEFAULT_SALT_LEN.
 */
static void scram_mock_salt(const char *username, uint8_t *saltbuf)
{
	static uint8_t mock_auth_nonce[32];
	static bool mock_auth_nonce_initialized = false;
	struct sha256_ctx ctx;
	uint8_t sha_digest[PG_SHA256_DIGEST_LENGTH];

	/*
	 * Generating salt using a SHA256 hash works as long as the
	 * required salt length is not larger than the SHA256 digest
	 * length.
	 */
	static_assert(PG_SHA256_DIGEST_LENGTH >= SCRAM_DEFAULT_SALT_LEN,
		      "salt length greater than SHA256 digest length");

	if (!mock_auth_nonce_initialized) {
		get_random_bytes(mock_auth_nonce, sizeof(mock_auth_nonce));
		mock_auth_nonce_initialized = true;
	}

	sha256_reset(&ctx);
	sha256_update(&ctx, (uint8_t *) username, strlen(username));
	sha256_update(&ctx, mock_auth_nonce, sizeof(mock_auth_nonce));
	sha256_final(&ctx, sha_digest);

	memcpy(saltbuf, sha_digest, SCRAM_DEFAULT_SALT_LEN);
}

static bool build_mock_scram_secret(const char *username, ScramState *scram_state)
{
	uint8_t saltbuf[SCRAM_DEFAULT_SALT_LEN];
	int encoded_len;

	scram_state->iterations = cf_scram_iterations;

	scram_mock_salt(username, saltbuf);
	encoded_len = pg_b64_enc_len(sizeof(saltbuf));
	scram_state->salt = malloc(encoded_len + 1);
	if (!scram_state->salt)
		goto failed;
	encoded_len = pg_b64_encode((char *) saltbuf, sizeof(saltbuf), scram_state->salt, encoded_len);
	if (encoded_len < 0)
		goto failed;
	scram_state->salt[encoded_len] = '\0';

	return true;
failed:
	return false;
}

char *build_server_first_message(ScramState *scram_state, PgCredentials *user, const char *stored_secret)
{
	uint8_t raw_nonce[SCRAM_RAW_NONCE_LEN + 1];
	int encoded_len;
	size_t len;
	char *result;

	if (!stored_secret) {
		if (!build_mock_scram_secret(user->name, scram_state))
			goto failed;
	} else {
		if (user->adhoc_scram_secrets_cached) {
			scram_state->iterations = user->scram_Iiterations;
			scram_state->salt = strdup(user->scram_SaltKey);
			memcpy(scram_state->StoredKey, user->scram_StoredKey, sizeof(user->scram_StoredKey));
			memcpy(scram_state->ServerKey, user->scram_ServerKey, sizeof(user->scram_ServerKey));
		} else {
			switch (get_password_type(stored_secret)) {
			case PASSWORD_TYPE_SCRAM_SHA_256:
				if (!parse_scram_secret(stored_secret,
							&scram_state->iterations,
							&scram_state->salt,
							scram_state->StoredKey,
							scram_state->ServerKey))
					goto failed;
				break;
			case PASSWORD_TYPE_PLAINTEXT:
				if (!build_adhoc_scram_secret(stored_secret, scram_state))
					goto failed;
				break;
			default:
				/* shouldn't get here */
				goto failed;
			}

			if (!user->dynamic_passwd) {
				user->scram_Iiterations = scram_state->iterations;
				user->scram_SaltKey = strdup(scram_state->salt);
				memcpy(user->scram_StoredKey, scram_state->StoredKey, sizeof(scram_state->StoredKey));
				memcpy(user->scram_ServerKey, scram_state->ServerKey, sizeof(scram_state->ServerKey));
				user->adhoc_scram_secrets_cached = true;
			}
		}
	}

	get_random_bytes(raw_nonce, SCRAM_RAW_NONCE_LEN);
	encoded_len = pg_b64_enc_len(SCRAM_RAW_NONCE_LEN);
	scram_state->server_nonce = malloc(encoded_len + 1);
	if (scram_state->server_nonce == NULL)
		goto failed;
	encoded_len = pg_b64_encode((char *) raw_nonce, SCRAM_RAW_NONCE_LEN, scram_state->server_nonce, encoded_len);
	if (encoded_len < 0)
		goto failed;
	scram_state->server_nonce[encoded_len] = '\0';

	len = (2
	       + strlen(scram_state->client_nonce)
	       + strlen(scram_state->server_nonce)
	       + 3
	       + strlen(scram_state->salt)
	       + 3 + 10 + 1);
	result = malloc(len);
	if (!result)
		goto failed;
	snprintf(result, len,
		 "r=%s%s,s=%s,i=%u",
		 scram_state->client_nonce,
		 scram_state->server_nonce,
		 scram_state->salt,
		 scram_state->iterations);

	scram_state->server_first_message = result;

	return result;
failed:
	free(scram_state->server_nonce);
	free(scram_state->server_first_message);
	return NULL;
}

static char *compute_server_signature(ScramState *state)
{
	uint8_t ServerSignature[SCRAM_KEY_LEN];
	char *server_signature_base64;
	int siglen;
	scram_HMAC_ctx ctx;

	/* calculate ServerSignature */
	scram_HMAC_init(&ctx, state->ServerKey, SCRAM_KEY_LEN);
	scram_HMAC_update(&ctx,
			  state->client_first_message_bare,
			  strlen(state->client_first_message_bare));
	scram_HMAC_update(&ctx, ",", 1);
	scram_HMAC_update(&ctx,
			  state->server_first_message,
			  strlen(state->server_first_message));
	scram_HMAC_update(&ctx, ",", 1);
	scram_HMAC_update(&ctx,
			  state->client_final_message_without_proof,
			  strlen(state->client_final_message_without_proof));
	scram_HMAC_final(ServerSignature, &ctx);

	siglen = pg_b64_enc_len(SCRAM_KEY_LEN);
	server_signature_base64 = malloc(siglen + 1);
	if (!server_signature_base64)
		return NULL;
	siglen = pg_b64_encode((const char *) ServerSignature,
			       SCRAM_KEY_LEN, server_signature_base64, siglen);
	if (siglen < 0) {
		free(server_signature_base64);
		return NULL;
	}
	server_signature_base64[siglen] = '\0';

	return server_signature_base64;
}

char *build_server_final_message(ScramState *scram_state)
{
	char *server_signature = NULL;
	size_t len;
	char *result;

	server_signature = compute_server_signature(scram_state);
	if (!server_signature)
		goto failed;

	len = 2 + strlen(server_signature) + 1;

	/*
	 * Avoid compiler warning at snprintf() below because len
	 * could in theory overflow snprintf() result.  If this
	 * happened in practice, it would surely be some crazy
	 * corruption, so treat it as an error.
	 */
	if (len >= INT_MAX)
		goto failed;

	result = malloc(len);
	if (!result)
		goto failed;
	snprintf(result, len, "v=%s", server_signature);

	free(server_signature);
	return result;
failed:
	free(server_signature);
	return NULL;
}

bool verify_final_nonce(const ScramState *scram_state, const char *client_final_nonce)
{
	size_t client_nonce_len = strlen(scram_state->client_nonce);
	size_t server_nonce_len = strlen(scram_state->server_nonce);
	size_t final_nonce_len = strlen(client_final_nonce);

	if (final_nonce_len != client_nonce_len + server_nonce_len)
		return false;
	if (memcmp(client_final_nonce, scram_state->client_nonce, client_nonce_len) != 0)
		return false;
	if (memcmp(client_final_nonce + client_nonce_len, scram_state->server_nonce, server_nonce_len) != 0)
		return false;

	return true;
}

bool verify_client_proof(ScramState *state, const char *ClientProof)
{
	uint8_t ClientSignature[SCRAM_KEY_LEN];
	uint8_t client_StoredKey[SCRAM_KEY_LEN];
	scram_HMAC_ctx ctx;
	int i;

	/* calculate ClientSignature */
	scram_HMAC_init(&ctx, state->StoredKey, SCRAM_KEY_LEN);
	scram_HMAC_update(&ctx,
			  state->client_first_message_bare,
			  strlen(state->client_first_message_bare));
	scram_HMAC_update(&ctx, ",", 1);
	scram_HMAC_update(&ctx,
			  state->server_first_message,
			  strlen(state->server_first_message));
	scram_HMAC_update(&ctx, ",", 1);
	scram_HMAC_update(&ctx,
			  state->client_final_message_without_proof,
			  strlen(state->client_final_message_without_proof));
	scram_HMAC_final(ClientSignature, &ctx);

	/* Extract the ClientKey that the client calculated from the proof */
	for (i = 0; i < SCRAM_KEY_LEN; i++)
		state->ClientKey[i] = ClientProof[i] ^ ClientSignature[i];

	/* Hash it one more time, and compare with StoredKey */
	scram_H(state->ClientKey, SCRAM_KEY_LEN, client_StoredKey);

	if (memcmp(client_StoredKey, state->StoredKey, SCRAM_KEY_LEN) != 0)
		return false;

	return true;
}

/*
 * Verify a plaintext password against a SCRAM secret.  This is used when
 * performing plaintext password authentication for a user that has a SCRAM
 * secret stored in pg_authid.
 */
bool scram_verify_plain_password(PgSocket *client,
				 const char *username, const char *password,
				 const char *secret)
{
	char *encoded_salt = NULL;
	char *salt = NULL;
	int saltlen;
	int iterations;
	uint8_t salted_password[SCRAM_KEY_LEN];
	uint8_t stored_key[SCRAM_KEY_LEN];
	uint8_t server_key[SCRAM_KEY_LEN];
	uint8_t computed_key[SCRAM_KEY_LEN];
	char *prep_password = NULL;
	pg_saslprep_rc rc;
	bool result = false;

	if (!parse_scram_secret(secret, &iterations, &encoded_salt,
				stored_key, server_key)) {
		/* The password looked like a SCRAM secret, but could not be parsed. */
		slog_warning(client, "invalid SCRAM secret for user \"%s\"", username);
		goto failed;
	}

	saltlen = pg_b64_dec_len(strlen(encoded_salt));
	salt = malloc(saltlen);
	if (!salt)
		goto failed;
	saltlen = pg_b64_decode(encoded_salt, strlen(encoded_salt), salt, saltlen);
	if (saltlen < 0) {
		slog_warning(client, "invalid SCRAM secret for user \"%s\"", username);
		goto failed;
	}

	/* Normalize the password */
	rc = pg_saslprep(password, &prep_password);
	if (rc == SASLPREP_SUCCESS)
		password = prep_password;

	/* Compute Server Key based on the user-supplied plaintext password */
	scram_SaltedPassword(password, salt, saltlen, iterations, salted_password);
	scram_ServerKey(salted_password, computed_key);

	/*
	 * Compare the secret's Server Key with the one computed from the
	 * user-supplied password.
	 */
	result = memcmp(computed_key, server_key, SCRAM_KEY_LEN) == 0;

failed:
	free(encoded_salt);
	free(salt);
	free(prep_password);
	return result;
}
