/* Copyright (c) 2012, 2022, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   Without limiting anything contained in the foregoing, this file,
   which is part of C Driver for MySQL (Connector/C), is also subject to the
   Universal FOSS Exception, version 1.0, a copy of which can be found at
   http://oss.oracle.com/licenses/universal-foss-exception.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file mysys/my_md5.cc
  Wrapper functions for OpenSSL.
*/

#include "my_md5.h"

#include <openssl/crypto.h>

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/evp.h>
#include <openssl/provider.h>
#else
#include <openssl/md5.h>
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */

static void my_md5_hash(unsigned char *digest, unsigned const char *buf,
                        int len) {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  /*
    EVP_Digest() is a wrapper around the EVP_DigestInit_ex(),
    EVP_Update() and EVP_Final_ex() functions.
  */
  EVP_Digest(buf, len, digest, nullptr, EVP_md5(), nullptr);
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  MD5_CTX ctx;
  MD5_Init(&ctx);
  MD5_Update(&ctx, buf, len);
  MD5_Final(digest, &ctx);
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
}

/**
    Wrapper function to compute MD5 message digest.

    @param [out] digest Computed MD5 digest
    @param [in] buf     Message to be computed
    @param [in] len     Length of the message
    @return             0 when MD5 hash function called successfully
                        1 when MD5 hash function doesn't called because of fips
   mode (ON/STRICT)
*/
int compute_md5_hash(char *digest, const char *buf, int len) {
  int retval = 0;

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  int fips_mode = EVP_default_properties_is_fips_enabled(nullptr) &&
                  OSSL_PROVIDER_available(nullptr, "fips");
#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
  int fips_mode = FIPS_mode();
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */

  /* If fips mode is ON/STRICT restricted method calls will result into abort,
   * skipping call. */
  if (fips_mode == 0) {
    my_md5_hash((unsigned char *)digest, (unsigned const char *)buf, len);
  } else {
    retval = 1;
  }
  return retval;
}

#ifdef WITH_WSREP

/* For certification we need to identify each row uniquely.
Generally this is done using PK but if table is created w/o PK
then a md5-hash (16 bytes) string is generated using the complete record.
Following functions act as helper function in generation of this md5-hash. */

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
void *wsrep_md5_init() {
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  EVP_DigestInit_ex2(ctx, EVP_md5(), nullptr);
  return (void *)ctx;
}

void wsrep_md5_update(void *ctx, char *buf, int len) {
  EVP_DigestUpdate((EVP_MD_CTX *)ctx, buf, len);
}

void wsrep_compute_md5_hash(unsigned char *digest, void *ctx) {
  unsigned int md_len;
  EVP_DigestFinal_ex((EVP_MD_CTX *)ctx, digest, &md_len);
  EVP_MD_CTX_free((EVP_MD_CTX *)ctx);
}

#else  /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
void *wsrep_md5_init() {
  MD5_CTX *ctx = new MD5_CTX();
  MD5_Init(ctx);
  return (void *)ctx;
}

void wsrep_md5_update(void *ctx, char *buf, int len) {
  MD5_Update((MD5_CTX *)(ctx), buf, len);
}

void wsrep_compute_md5_hash(unsigned char *digest, void *ctx) {
  MD5_Final(digest, (MD5_CTX *)ctx);
  delete (MD5_CTX *)ctx;
}
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */

#endif /* WITH_WSREP */
 