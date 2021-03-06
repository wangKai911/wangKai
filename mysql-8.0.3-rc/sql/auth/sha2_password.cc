/*
   Copyright (c) 2017 Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "i_sha2_password.h"            /* Internal classes            */
#include "auth_internal.h"              /* Rsa_authentication_keys     */
#include "sql_auth_cache.h"             /* ACL_USER                    */
#include "sql_authentication.h"
#include "my_inttypes.h"                /* typedefs                    */
#include "my_dbug.h"                    /* DBUG instrumentation        */
#include "mysql/plugin_auth_common.h"   /* MYSQL_PLUGIN_VIO            */
#include "mysql/plugin_auth.h"          /* MYSQL_SERVER_AUTH_INFO      */
#include "mysql/service_my_plugin_log.h"/* plugin_log_level            */
#include "mysql/service_mysql_password_policy.h"
                                        /* my_validate_password_policy */
#include <sstream>                      /* std::stringstream           */
#include <iomanip>                      /* std::setfill(), std::setw() */
#include <iostream>                     /* For debugging               */
#include <sql/sql_const.h>              /* MAX_FIELD_WIDTH             */
#include <sql/protocol_classic.h>       /* Protocol_classic            */
#include "rwlock_scoped_lock.h"         /* rwlock_scoped_lock          */

#if defined(HAVE_YASSL)
#include <openssl/ssl.h>
#endif

char *caching_sha2_rsa_private_key_path;
char *caching_sha2_rsa_public_key_path;
Rsa_authentication_keys *g_caching_sha2_rsa_keys= 0;

namespace sha2_password
{
  using std::min;

  /** Destructor - Release all memory */
  SHA2_password_cache::~SHA2_password_cache()
  {
    clear_cache();
    password_cache empty;
    m_password_cache.swap(empty);
  }


  /**
    Add an entry in cache
    We manage our own memory

    @param [in] authorization_id   Key
    @param [in] entry_to_be_cached Value

    @returns status of addition
      @retval false Successful insertion
      @retval true Error
  */

  bool
  SHA2_password_cache::add(const std::string authorization_id,
                           const sha2_cache_entry &entry_to_be_cached)
  {
    DBUG_ENTER("SHA2_password_cache::add");
    auto ret= m_password_cache.insert(std::pair<std::string, sha2_cache_entry>
                                     (authorization_id, entry_to_be_cached));
    if (ret.second == false)
      DBUG_RETURN(true);

    DBUG_RETURN(false);
  }


  /**
    Remove an entry from the cache

    @param [in] authorization_id  AuthID to search against

    @return out of the deletion
      @retval false Entry successfully removed
      @retval true Error removing the entry
  */

  bool
  SHA2_password_cache::remove(const std::string authorization_id)
  {
    DBUG_ENTER("SHA2_password_cache::remove");
    auto it= m_password_cache.find(authorization_id);
    if (it != m_password_cache.end())
    {
      m_password_cache.erase(it);
      DBUG_RETURN(false);
    }
    DBUG_RETURN(true);
  }


  /**
    Search an entry from the cache

    @param [in]  authorization_id   AuthID to search against
    @param [out] cache_entry Stored Password for given AuthID

    Assumption : Memory for password is allocated by the caller.

    @returns Status of search operation
      @retval false Entry found. Password contains the stored credential
      @retval true Entry not found.
  */

  bool SHA2_password_cache::search(const std::string authorization_id,
                                   sha2_cache_entry &cache_entry)
  {
    DBUG_ENTER("SHA2_password_cache::search");
    auto it= m_password_cache.find(authorization_id);
    if (it != m_password_cache.end())
    {
      const sha2_cache_entry stored_entry= it->second;
      memcpy(cache_entry.digest_buffer, stored_entry.digest_buffer,
             sizeof(cache_entry.digest_buffer));
      DBUG_RETURN(false);
    }
    DBUG_RETURN(true);
  }


  /** Clear the cache - Release all memory */
  void SHA2_password_cache::clear_cache()
  {
    if (!m_password_cache.empty())
      m_password_cache.clear();
  }


  static const char *category= "sha2_auth";
  static PSI_rwlock_key key_m_cache_lock;
  static PSI_rwlock_info all_rwlocks[]=
  {
    {&key_m_cache_lock, "key_m_cache_lock", 0, 0, PSI_DOCUMENT_ME}
  };


  /**
    Caching_sha2_password constructor - Initializes rw lock

    @param [in] plugin_handle        MYSQL_PLUGIN reference
    @param [in] stored_digest_rounds Number of rounds for
                                     stored digest generation
    @param [in] fast_digest_rounds   Number of rounds for
                                     fast digest generation
    @param [in] digest_type          SHA2 type to be used
  */

  Caching_sha2_password::Caching_sha2_password(
    MYSQL_PLUGIN plugin_handle,
    size_t stored_digest_rounds,        /* = DEFAULT_STORED_DIGEST_ROUNDS */
    unsigned int fast_digest_rounds,    /* = DEFAULT_FAST_DIGEST_ROUNDS */
    Digest_info digest_type)            /* = Digest_info::SHA256_DIGEST */
    : m_plugin_info(plugin_handle),
      m_stored_digest_rounds(stored_digest_rounds),
      m_fast_digest_rounds(fast_digest_rounds),
      m_digest_type(digest_type)
  {
    int count= array_elements(all_rwlocks);
    mysql_rwlock_register(category, all_rwlocks, count);
    mysql_rwlock_init(key_m_cache_lock, &m_cache_lock);

    if (fast_digest_rounds > MAX_FAST_DIGEST_ROUNDS ||
        fast_digest_rounds < MIN_FAST_DIGEST_ROUNDS)
      m_fast_digest_rounds= DEFAULT_FAST_DIGEST_ROUNDS;


    if (stored_digest_rounds > MAX_STORED_DIGEST_ROUNDS ||
        stored_digest_rounds < MIN_STORED_DIGEST_ROUNDS)
      m_stored_digest_rounds= DEFAULT_STORED_DIGEST_ROUNDS;
  }


  /**
    Caching_sha2_password destructor - destroy rw lock
  */
  Caching_sha2_password::~Caching_sha2_password()
  {
    mysql_rwlock_destroy(&m_cache_lock);
  }


  /**
    Perform slow authentication.

    1. Disect serialized_string and retrieve
      a. Salt
      b. Hash iteration count
      c. Expected hash
    2. Use plaintext password, salt and hash iteration count to generate
       hash
    3. Validate generated hash against expected hash

    In case of successful authentication, update password cache

    @param [in] authorization_id   User information
    @param [in] serialized_string        Information retrieved from
                                   mysql.authentication_string column
    @param [in] plaintext_password Password as received from client

    @returns Outcome of comparison against expected hash
      @retval false Success. Entry may have been added in cache.
      @retval true  Validation error.
  */

  bool Caching_sha2_password::authenticate(const std::string &authorization_id,
                                           const std::string &serialized_string,
                                           const std::string &plaintext_password)
  {
    DBUG_ENTER("Caching_sha2_password::authenticate");
    /* Empty authentication string */
    if (!serialized_string.length())
      DBUG_RETURN(plaintext_password.length() ? true : false);

    std::string random;
    std::string digest;
    std::string generated_digest;
    Digest_info digest_type;
    size_t iterations;

    /*
      Get digest type, iterations, salt and digest
      from the authentication string.
    */
    if (deserialize(serialized_string, digest_type,
                    random, digest, iterations))
    {
      if (m_plugin_info)
        my_plugin_log_message(&m_plugin_info, MY_ERROR_LEVEL,
          "Failed to parse stored authentication string for %s."
          "Please check if mysql.user table not corrupted.",
          authorization_id.c_str());
      DBUG_RETURN(true);
    }

    /*
      Generate multiple rounds of sha2 hash using plaintext password
      and salt retrieved from the authentication string.
    */

    if (this->generate_sha2_multi_hash(plaintext_password, random,
                                       &generated_digest,
                                       m_stored_digest_rounds))
    {
      if (m_plugin_info)
        my_plugin_log_message(&m_plugin_info, MY_ERROR_LEVEL,
          "Error in generating multi-round hash for %s."
          "Plugin can not perform authentication without it."
          "This may be a transient problem.",
          authorization_id.c_str());
      DBUG_RETURN(true);
    }

    /*
      Generated digest should match with stored digest
      for successful authentication.
    */
    if (memcmp(digest.c_str(), generated_digest.c_str(),
               STORED_SHA256_DIGEST_LENGTH) == 0)
    {
      /*
        If authentication is successful, we would want to make
        entry in cache for fast authentication. Subsequent
        authentication attempts would use the fast authentication
        to speed up the process.
      */
      sha2_cache_entry fast_digest;

      if (generate_fast_digest(plaintext_password, fast_digest))
      {
        DBUG_PRINT("info", ("Failed to generate multi-round hash for %s. "
                            "Fast authentication won't be possible.",
                            authorization_id.c_str()));
        DBUG_RETURN(false);
      }

      rwlock_scoped_lock wrlock(&m_cache_lock, true, __FILE__, __LINE__);
      if (m_cache.add(authorization_id, fast_digest))
      {
        sha2_cache_entry stored_digest;
        m_cache.search(authorization_id, stored_digest);

        /* Same digest is already added, so just return */
        if (memcmp(fast_digest.digest_buffer, stored_digest.digest_buffer,
            sizeof(fast_digest.digest_buffer)) == 0)
          DBUG_RETURN(false);

        /* Update the digest */
        m_cache.remove(authorization_id);
        m_cache.add(authorization_id, fast_digest);
        DBUG_PRINT("info", ("An old digest for %s was recorded in cache. "
                            "It has been replaced with the latest digest.",
                            authorization_id.c_str()));
        DBUG_RETURN(false);
      }
      DBUG_RETURN(false);
    }
    DBUG_RETURN(true);
  }


  /**
    Perform fast authentication

    1. Retrieve hash from cache
    2. Validate it against received scramble

    @param [in] authorization_id User information
    @param [in] random           Per session random number
    @param [in] random_length    Length of the random number
    @param [in] scramble         Scramble received from the client

    @returns Outcome of scramble validation
      @retval false Success.
      @retval true Validation error.
  */

  bool Caching_sha2_password::
    fast_authenticate(const std::string &authorization_id,
                      const unsigned char *random,
                      unsigned int random_length,
                      const unsigned char *scramble)
  {
    DBUG_ENTER("Caching_sha2_password::fast_authenticate");
    if (!scramble || !random)
    {
      DBUG_PRINT("info", ("For authorization id : %s,"
                          "Scramble is null - %s :"
                          "Random is null - %s :",
                          authorization_id.c_str(),
                          !scramble ? "true" : "false",
                          !random ? "true" : "false"));
      DBUG_RETURN(true);
    }

    rwlock_scoped_lock rdlock(&m_cache_lock, false, __FILE__, __LINE__);
    sha2_cache_entry digest;

    if (m_cache.search(authorization_id, digest))
    {
      DBUG_PRINT("info", ("Could not find entry for %s in cache.",
                          authorization_id.c_str()));
      DBUG_RETURN(true);
    }

    /* Entry found, so validate scramble against it */
    Validate_scramble validate_scramble(scramble, digest.digest_buffer,
                                        random, random_length);
    DBUG_RETURN(validate_scramble.validate());
  }


  /**
    Remove an entry from the cache.

    This can happen due to one of the following:
    a. DROP USER
    b. RENAME USER

    @param [in] authorization_id User name
  */

  void Caching_sha2_password::remove_cached_entry(const std::string authorization_id)
  {
    rwlock_scoped_lock wrlock(&m_cache_lock, true, __FILE__, __LINE__);
    /* It is possible that entry is not present at all, but we don't care */
    (void)m_cache.remove(authorization_id);
  }


  /**
    Deserialize obtained hash and retrieve various parts.

    From stored string, following parts are retrieved:
      Digest type
      Salt
      Iteration count
      hash

    Expected format
    DELIMITER[digest_type]DELIMITER[iterations]DELIMITER[salt][digest]

    digest_type:
    A => SHA256

    iterations:
    005 => 5*ITERATION_MULTIPLIER

    salt:
    Random string. Length SALT_LENGTH

    digest:
    SHA2 digest. Length STORED_SHA256_DIGEST_LENGTH

    @param [in]  serialized_string serialized string
    @param [out] digest_type       Digest algorithm
    @param [out] salt              Random string used for hashing
    @param [out] digest            Digest stored
    @param [out] iterations        Number of hash iterations

    @returns status of parsing
      @retval false. Success. out variables updated.
      @retval true. Failure. out variables should not be used.
  */

  bool Caching_sha2_password::deserialize(const std::string &serialized_string,
                                          Digest_info &digest_type,
                                          std::string &salt,
                                          std::string &digest,
                                          size_t &iterations)
  {
    DBUG_ENTER("Caching_sha2_password::deserialize");
    if (!serialized_string.length())
      DBUG_RETURN(true);
    /* Digest Type */
    std::string::size_type delimiter= serialized_string.find(DELIMITER, 0);
    if (delimiter == std::string::npos)
    {
      DBUG_PRINT("info", ("Digest string is not in expected format."));
      DBUG_RETURN(true);
    }
    std::string digest_type_info= serialized_string.substr(delimiter + 1, DIGEST_INFO_LENGTH);
    if (digest_type_info == "A")
      digest_type= Digest_info::SHA256_DIGEST;
    else
    {
      DBUG_PRINT("info", ("Digest string is not in expected format."
                          "Missing digest type information."));
      DBUG_RETURN(true);
    }

    /* Iteration */
    delimiter= serialized_string.find(DELIMITER, delimiter + 1);
    if (delimiter == std::string::npos)
    {
      DBUG_PRINT("info", ("Digest string is not in expected format."
                          "Missing iteration count information."));
      DBUG_RETURN(true);
    }
    std::string::size_type delimiter_2= serialized_string.find(DELIMITER, delimiter + 1);
    if (delimiter_2 == std::string::npos ||
        delimiter_2 - delimiter != 4)
    {
      DBUG_PRINT("info", ("Digest string is not in expected format."
                          "Invalid iteration count information."));
      DBUG_RETURN(true);
    }
    std::string iteration_info= serialized_string.substr(delimiter + 1,
                                                         ITERATION_LENGTH);
    iterations= std::min((std::stoul(iteration_info, nullptr))*ITERATION_MULTIPLIER,
                         MAX_ITERATIONS);
    if (!iterations)
    {
      DBUG_PRINT("info", ("Digest string is not in expected format."
                          "Invalid iteration count information."));
      DBUG_RETURN(true);
    }

    /* Salt */
    delimiter= delimiter_2;
    salt= serialized_string.substr(delimiter + 1, SALT_LENGTH);
    if (salt.length() != SALT_LENGTH)
    {
      DBUG_PRINT("info", ("Digest string is not in expected format."
                          "Invalid salt information."));
      DBUG_RETURN(true);
    }

    /* Digest */
    digest= serialized_string.substr(delimiter + 1 + SALT_LENGTH,
                                     std::string::npos);
    switch(digest_type)
    {
      case Digest_info::SHA256_DIGEST:
        if (digest.length() != STORED_SHA256_DIGEST_LENGTH)
        {
          DBUG_PRINT("info", ("Digest string is not in expected format."
                              "Invalid digest length."));
          DBUG_RETURN(true);
        }
        break;
      default:
        DBUG_RETURN(true);
    };
    DBUG_RETURN(false);
  }


  /**
    Serialize following:
      a. Digest type
      b. Iteration count
      c. Salt
      d. Hash
    Expected output format:
    DELIMITER[digest_type]DELIMITER[iterations]DELIMITER[salt][digest]

    digest_type:
    A => SHA256

    iterations:
    5000 => 005

    salt:
    Random string. Length CRYPT_SALT_LENGTH

    digest:
    SHA2 digest. Length STORED_SHA256_DIGEST_LENGTH

    @param [out] serialized_string String to be stored
    @param [in]  digest_type       Digest algorithm
    @param [in]  salt              Random string used for hashing
    @param [in]  digest            Generated Digest
    @param [in]  iterations        Number of hash iterations
  */

  bool Caching_sha2_password::serialize(std::string &serialized_string,
                                        const Digest_info &digest_type,
                                        const std::string &salt,
                                        const std::string &digest,
                                        size_t iterations)
  {
    DBUG_ENTER("Caching_sha2_password::serialize");
    std::stringstream ss;
    /* Digest type */
    switch (digest_type)
    {
      case Digest_info::SHA256_DIGEST:
        ss << DELIMITER << "A" << DELIMITER;
        break;
      default:
        DBUG_RETURN(true);
    }

    /* Iterations */
    unsigned int iteration_info= iterations/ITERATION_MULTIPLIER;
    if (!iteration_info || iterations > MAX_ITERATIONS)
    {
      DBUG_PRINT("info", ("Invalid iteration count information."));
      DBUG_RETURN(true);
    }
    ss << std::setfill('0') << std::setw(3) << iteration_info << DELIMITER;
    serialized_string= ss.str();

    /* Salt */
    if (salt.length() != SALT_LENGTH)
    {
      DBUG_PRINT("info", ("Invalid salt."));
      DBUG_RETURN(true);
    }
    serialized_string.append(salt.c_str(), salt.length());

    /* Digest */
    switch (digest_type)
    {
      case Digest_info::SHA256_DIGEST:
        if (digest.length() != STORED_SHA256_DIGEST_LENGTH)
        {
          DBUG_PRINT("info", ("Invalid digest size."));
          DBUG_RETURN(true);
        }
        serialized_string.append(digest.c_str(), digest.length());
        break;
      default:
        DBUG_RETURN(true);
    };
    DBUG_RETURN(false);
  }


  /**
    Generate digest based on m_fast_digest_rounds

    @param [out] digest Digest output buffer
    @param [in]  plaintext_password Source text

    @returns status of digest generation
      @retval false Success.
      @retval true Error. Don't rely on digest.
  */

  bool Caching_sha2_password::generate_fast_digest(const std::string &plaintext_password,
                                                   sha2_cache_entry &digest)
  {
    DBUG_ENTER("Caching_sha2_password::generate_fast_digest");
    SHA256_digest sha256_digest;
    unsigned char digest_buffer[CACHING_SHA2_DIGEST_LENGTH];

    if (sha256_digest.update_digest(plaintext_password.c_str(),
                                    plaintext_password.length()) ||
        sha256_digest.retrieve_digest(digest_buffer, CACHING_SHA2_DIGEST_LENGTH))
    {
      DBUG_PRINT("info", ("Failed to generate SHA256 digest for password"));
      DBUG_RETURN(true);
    }

    for (unsigned int i=1; i < m_fast_digest_rounds; ++i)
    {
      sha256_digest.scrub();
      if (sha256_digest.update_digest(digest_buffer, CACHING_SHA2_DIGEST_LENGTH) ||
          sha256_digest.retrieve_digest(digest_buffer, CACHING_SHA2_DIGEST_LENGTH))
      {
        DBUG_PRINT("info", ("Failed to generate SHA256 of SHA256 "
                            "digest for password"));
        DBUG_RETURN(true);
      }
    }

    /* Calculated digest is stored in digest */
    memcpy(digest.digest_buffer, digest_buffer, sizeof(digest.digest_buffer));
    DBUG_RETURN(false);
  }


  /**
    Generate multi-round sha2 hash using source and random string.
    This is a wrapper around my_crypt_genhash

    @param [in]  source    Source text
    @param [in]  random    Random text
    @param [out] digest    Generated sha2 digest
    @param [in] iterations Number of hash iterations

    @returns result of password check
      @retval false Password matches
      @retval true  Password does not match
  */

  bool Caching_sha2_password::generate_sha2_multi_hash(const std::string &source,
                                                       const std::string &random,
                                                       std::string *digest,
                                                       unsigned int iterations)
  {
    DBUG_ENTER("Caching_sha2_password::generate_sha2_multi_hash");
    char salt[SALT_LENGTH + 1];
    /* Generate salt including terminating \0 */
    generate_user_salt(salt, SALT_LENGTH + 1);

    switch(m_digest_type)
    {
      case Digest_info::SHA256_DIGEST:
      {
        char buffer[CRYPT_MAX_PASSWORD_SIZE + 1];
        memset(buffer, 0, sizeof(buffer));
        my_crypt_genhash(buffer, CRYPT_MAX_PASSWORD_SIZE,
                         source.c_str(), source.length(),
                         random.c_str(), nullptr, &iterations);

        /*
          Returned value in buffer would be in format:
          $5$<SALT_LENGTH><STORED_SHA256_DIGEST_LENGTH>
          We need to extract STORED_SHA256_DIGEST_LENGTH chars from it
        */
        digest->assign(buffer + 3 + SALT_LENGTH + 1, STORED_SHA256_DIGEST_LENGTH);
        break;
      }
      default:
        DBUG_ASSERT(FALSE);
        DBUG_RETURN(true);
    }
    DBUG_RETURN(false);
  }


  /**
    Get cache count

    @returns number of elements in the cache
  */

  size_t Caching_sha2_password::get_cache_count()
  {
    DBUG_ENTER("Caching_sha2_password::get_cache_count");
    rwlock_scoped_lock rdlock(&m_cache_lock, false, __FILE__, __LINE__);
    DBUG_RETURN(m_cache.size());
  }


  /** Clear the password cache */
  void Caching_sha2_password::clear_cache()
  {
    DBUG_ENTER("Caching_sha2_password::clear_cache");
    rwlock_scoped_lock wrlock(&m_cache_lock, true, __FILE__, __LINE__);
    m_cache.clear_cache();
    DBUG_VOID_RETURN;
  }


  /**
    Validate a hash format

    @param [in] serialized_string Supplied hash

    @returns result of validation
      @retval false Valid hash
      @retval true  Invalid hash
  */
  bool
  Caching_sha2_password::validate_hash(const std::string serialized_string)
  {
    DBUG_ENTER("Caching_sha2_password::validate_hash");
    Digest_info digest_type;
    std::string salt;
    std::string digest;
    size_t iterations;

    if (!serialized_string.length())
    {
      DBUG_PRINT("info", ("0 length digest."));
      DBUG_RETURN(false);
    }

    DBUG_RETURN(deserialize(serialized_string, digest_type,
                            salt, digest, iterations));
  }

} // sha2_password

/** Length of encrypted packet */
const int MAX_CIPHER_LENGTH= 1024;

/** Default iteration count */
const int CACHING_SHA2_PASSWORD_ITERATIONS= sha2_password::DEFAULT_STORED_DIGEST_ROUNDS;

/** Caching_sha2_password handle */
sha2_password::Caching_sha2_password *g_caching_sha2_password= 0;

/** caching_sha2_password name */
LEX_CSTRING caching_sha2_password_plugin_name= {
  C_STRING_WITH_LEN("caching_sha2_password")
};


/** caching_sha2_password plugin handle - Mostly used for logging */
static MYSQL_PLUGIN caching_sha2_auth_plugin_ref;


/** Interface for querying the MYSQL_PUBLIC_VIO about encryption state */
static int my_vio_is_secure(MYSQL_PLUGIN_VIO *vio)
{
  MPVIO_EXT *mpvio= (MPVIO_EXT *) vio;
  return is_secure_transport(mpvio->protocol->get_vio()->type);
}


/**
  Check if server has valid public key/private key
  pair for RSA communication.

  @return
    @retval false RSA support is available
    @retval true RSA support is not available
*/
bool caching_sha2_rsa_auth_status()
{
      return (!g_caching_sha2_rsa_keys->get_private_key() ||
              !g_caching_sha2_rsa_keys->get_public_key());
}


/**
  Save the scramble in mpvio for future re-use.

  It is useful when we need to pass the scramble to another plugin.
  Especially in case when old 5.1 client with no CLIENT_PLUGIN_AUTH capability
  tries to connect to server with default-authentication-plugin set to
  caching_sha2_password

  @param vio      Virtual Input-Output interface
  @param scramble Scramble to be saved
*/

void static inline auth_save_scramble(MYSQL_PLUGIN_VIO *vio, const char *scramble)
{
  MPVIO_EXT *mpvio= (MPVIO_EXT *) vio;
  strncpy(mpvio->scramble, scramble, SCRAMBLE_LENGTH + 1);
}


/**
  Make hash key

  @param [in] username User part of the key
  @param [in] hostname Host part of the key
  @param [out] key     Generated hash key
*/
static void make_hash_key(const char *username,
                          const char *hostname,
                          std::string &key)
{
  DBUG_ENTER("make_hash_key");
  key.clear();
  key.append(username ? username : "");
  key.push_back('\0');
  key.append(hostname ? hostname : "");
  key.push_back('\0');
  DBUG_VOID_RETURN;
}

static char request_public_key= '\2';
static char fast_auth_success= '\3';
static char perform_full_authentication= '\4';

/**
  @page page_caching_sha2_authentication_exchanges Caching_sha2_password Message exchanges

  Definitation:

  Nonce - Random number generated by server
  password - password supplied by client
  Scramble - XOR(SHA2(password), SHA2(SHA2(SHA2(password)), Nonce))

  Case 1A : Authentication success with full authentication - TLS connection
  --------------------------------------------------------------------------
  Server -> Client : Send nonce
  Client -> Server : Send Scramble
  Server -> Client : Send perform_full_authentication
  Client -> Server : Send password
  Server -> Client : Send OK - Authentication success

  Case 1B : Authentication success with full authentication
            Plaintext connection | RSA Keys
  ---------------------------------------------------------
  Server -> Client : Send nonce
  Client -> Server : Send Scramble
  Server -> Client : Send perform_full_authentication
  Client -> Server : Send XOR(Password, Nonce) encrypted with public key
  Server -> Client : Send OK - Authentication success

  Case 1C : Authentication success with full authentication
            Plaintext connection | --get-server-public-key set by client
  ----------------------------------------------------------------------
  Server -> Client : Send nonce
  Client -> Server : Send Scramble
  Server -> Client : Send perform_full_authentication
  Client -> Server : Send request_server_public_key
  Server -> Client : Send public key
  Client -> Server : Send XOR(Password, Nonce) encrypted with public key
  Server -> Client : Send OK - Authentication success

  Case 2A : Authentication failure with full authentication - TLS connection
  AND
  Case 3A : Authentication failure with fast authentication - TLS connection
  --------------------------------------------------------------------------
  Server -> Client : Send nonce
  Client -> Server : Send Scramble
  Server -> Client : Send perform_full_authentication
  Client -> Server : Send password
  Server -> Client : Send OK - Authentication failure

  Case 2B : Authentication failure with full authentication
            Plaintext connection | RSA Keys
  AND
  Case 3B : Authentication failure with fast authentication
            Plaintext connection | RSA Keys
  ---------------------------------------------------------
  Server -> Client : Send nonce
  Client -> Server : Send Scramble
  Server -> Client : Send perform_full_authentication
  Client -> Server : Send XOR(Password, Nonce) encrypted with public key
  Server -> Client : Send OK - Authentication failure

  Case 2C : Authentication failure with full authentication
            Plaintext connection | --get-server-public-key set by client
  AND
  Case 3C : Authentication success with fast authentication
            Plaintext connection | --get-server-public-key set by client
  ----------------------------------------------------------------------
  Server -> Client : Send nonce
  Client -> Server : Send Scramble
  Server -> Client : Send perform_full_authentication
  Client -> Server : Send request_server_public_key
  Server -> Client : Send public key
  Client -> Server : Send XOR(Password, Nonce) encrypted with public key
  Server -> Client : Send OK - Authentication failure

  Case 2D : Authentication failure with full authentication - Plaintext connection
  AND
  Case 3D : Authentication failure with fast authentication - Plaintext connection
  --------------------------------------------------------------------------------
  Server -> Client : Send nonce
  Client -> Server : Send Scramble
  Server -> Client : Send perform_full_authentication
  Client -> Server : Connection termination because:
                     - Connection is not encrypted AND
                     - Client does not have public key AND
                     - --get-server-public-key is not set

  Case 4  : Authentication success with fast authentication - Any connection
  --------------------------------------------------------------------------
  Server -> Client : Send nonce
  Client -> Server : Send Scramble
  Server -> Client : Send OK - Authentication success

  Case 5A : Authentication success with empty password - Any connection
  ---------------------------------------------------------------------
  Server -> Client : Send nonce
  Client -> Server : Send empty response
  Server -> Client : Send OK - Authentication success

  Case 5B : Authentication failure with empty password - Any connection
  ---------------------------------------------------------------------
  Server -> Client : Send nonce
  Client -> Server : Send empty response
  Server -> Client : Send OK - Authentication failure
*/

/**
  Authentication routine for caching_sha2_password.

  @param [in] vio  Virtual I/O interface
  @param [in] info Connection information

  Refer to @ref page_caching_sha2_authentication_exchanges
  for server-client communication in various cases

  @returns status of authentication process
    @retval CR_OK    Successful authentication
    @retval CR_ERROR Authentication failure
*/

static int caching_sha2_password_authenticate(MYSQL_PLUGIN_VIO *vio,
                                              MYSQL_SERVER_AUTH_INFO *info)
{
  DBUG_ENTER("caching_sha2_password_authenticate");
  uchar *pkt;
  int pkt_len;
  char scramble[SCRAMBLE_LENGTH + 1];
  int cipher_length= 0;
  unsigned char plain_text[MAX_CIPHER_LENGTH + 1];
  RSA *private_key= NULL;
  RSA *public_key= NULL;

  generate_user_salt(scramble, SCRAMBLE_LENGTH + 1);

  /*
    Note: The nonce is split into 8 + 12 bytes according to
    http://dev.mysql.com/doc/internals/en/connection-phase-packets.html#packet-Protocol::HandshakeV10
    Native authentication sent 20 bytes + '\0' character = 21 bytes.
    This plugin must do the same to stay consistent with historical behavior
    if it is set to operate as a default plugin.
  */
  if (vio->write_packet(vio, (unsigned char *) scramble, SCRAMBLE_LENGTH + 1))
    DBUG_RETURN(CR_AUTH_HANDSHAKE);

  /*
    Save the scramble so it could be used by native plugin in case
    the authentication on the server side needs to be restarted
  */
  auth_save_scramble(vio, scramble);

  /*
    After the call to read_packet() the user name will appear in
    mpvio->acl_user and info will contain current data.
  */
  if ((pkt_len= vio->read_packet(vio, &pkt)) == -1)
    DBUG_RETURN(CR_AUTH_HANDSHAKE);

  /*
    If first packet is a 0 byte then the client isn't sending any password
    else the client will send a password.
  */
  if (!pkt_len || (pkt_len == 1 && *pkt == 0))
  {
    info->password_used= PASSWORD_USED_NO;
    /*
      Send OK signal; the authentication might still be rejected based on
      host mask.
    */
    if (info->auth_string_length == 0)
      DBUG_RETURN(CR_OK);
    else
      DBUG_RETURN(CR_AUTH_USER_CREDENTIALS);
  }
  else
    info->password_used= PASSWORD_USED_YES;

  MPVIO_EXT *mpvio= (MPVIO_EXT *) vio;
  std::string authorization_id;
  const char *hostname= mpvio->acl_user->host.get_host();
  make_hash_key(info->authenticated_as, hostname ? hostname : NULL,
                authorization_id);

  if (pkt_len != sha2_password::CACHING_SHA2_DIGEST_LENGTH)
    DBUG_RETURN(CR_ERROR);

  if (g_caching_sha2_password->fast_authenticate(authorization_id,
        reinterpret_cast<unsigned char *>(scramble), SCRAMBLE_LENGTH, pkt))
  {
    /*
      We either failed to authenticate or did not find entry in the cache.
      In either case, move to full authentication and ask the password
    */
    if (vio->write_packet(vio, (uchar *) &perform_full_authentication, 1))
      DBUG_RETURN(CR_AUTH_HANDSHAKE);
  }
  else
  {
    /* Send fast_auth_success packet followed by CR_OK */
    if (vio->write_packet(vio, (uchar *) &fast_auth_success, 1))
      DBUG_RETURN(CR_AUTH_HANDSHAKE);

    DBUG_RETURN(CR_OK);
  }

  /*
    Read packet from client - It will either be request for public key
    or password. We expect the pkt_len to be at least 1 because an empty
    password is '\0'.
    See setting of plaintext_password using unencrypted vio.
  */
  if ((pkt_len= vio->read_packet(vio, &pkt)) <= 0)
    DBUG_RETURN(CR_AUTH_HANDSHAKE);

  if (!my_vio_is_secure(vio))
  {
    /*
      Since a password is being used it must be encrypted by RSA since no
      other encryption is being active.
    */
    private_key= g_caching_sha2_rsa_keys->get_private_key();
    public_key=  g_caching_sha2_rsa_keys->get_public_key();

    /* Without the keys encryption isn't possible. */
    if (private_key == NULL || public_key == NULL)
    {
      if (caching_sha2_auth_plugin_ref)
        my_plugin_log_message(&caching_sha2_auth_plugin_ref, MY_ERROR_LEVEL,
          "Authentication requires either RSA keys or SSL encryption");
      DBUG_RETURN(CR_ERROR);
    }

    if ((cipher_length= g_caching_sha2_rsa_keys->get_cipher_length()) > MAX_CIPHER_LENGTH)
    {
      if (caching_sha2_auth_plugin_ref)
        my_plugin_log_message(&caching_sha2_auth_plugin_ref, MY_ERROR_LEVEL,
          "RSA key cipher length of %u is too long. Max value is %u.",
          g_caching_sha2_rsa_keys->get_cipher_length(), MAX_CIPHER_LENGTH);
      DBUG_RETURN(CR_ERROR);
    }

    /*
      Client sent a "public key request"-packet ?
      If the first packet is 1 then the client will require a public key before
      encrypting the password.
    */
    if (pkt_len == 1 && *pkt == request_public_key)
    {
      uint pem_length=
        static_cast<uint>(strlen(g_caching_sha2_rsa_keys->get_public_key_as_pem()));
      if (vio->write_packet(vio,
            (unsigned char *)g_caching_sha2_rsa_keys->get_public_key_as_pem(),
            pem_length))
        DBUG_RETURN(CR_ERROR);
      /* Get the encrypted response from the client */
      if ((pkt_len= vio->read_packet(vio, &pkt)) <= 0)
        DBUG_RETURN(CR_ERROR);
    }

    /*
      The packet will contain the cipher used. The length of the packet
      must correspond to the expected cipher length.
    */
    if (pkt_len != cipher_length)
      DBUG_RETURN(CR_ERROR);

    /* Decrypt password */
    RSA_private_decrypt(cipher_length, pkt, plain_text, private_key,
                        RSA_PKCS1_PADDING);

    plain_text[cipher_length]= '\0'; // safety
    xor_string((char *) plain_text, cipher_length,
               (char *) scramble, SCRAMBLE_LENGTH);

    /* Set packet pointers and length for the hash digest function below */
    pkt= plain_text;
    pkt_len= strlen((char *) plain_text) + 1; // include \0 intentionally.

    if (pkt_len == 1)
      DBUG_RETURN(CR_AUTH_USER_CREDENTIALS);
  } // if(!my_vio_is_encrypted())

  /* A password was sent to an account without a password */
  if (info->auth_string_length == 0)
    DBUG_RETURN(CR_AUTH_USER_CREDENTIALS);

  /* Fetch user authentication_string and extract the password salt */
  std::string serialized_string(info->auth_string, info->auth_string_length);
  std::string plaintext_password((char*)pkt, pkt_len-1);
  if (g_caching_sha2_password->authenticate(authorization_id,
                                            serialized_string,
                                            plaintext_password))

    DBUG_RETURN(CR_AUTH_USER_CREDENTIALS);

  DBUG_RETURN(CR_OK);
}


/**
  Generate hash for caching_sha2_password plugin

  @param [out] outbuf   Hash output buffer
  @param [out] buflen   Length of hash in output buffer
  @param [in]  inbuf    Plaintext password
  @param [in]  inbuflen Input password length

  @note outbuf must be larger than MAX_FIELD_WIDTH.
        It is assumed the caller asserts this.

  @returns status of hash generation
    @retval 0 Successful hash generation
    @retval 1 Error generating hash. Don't reply on outbuf/buflen
*/

static int caching_sha2_password_generate(char *outbuf,
                                          unsigned int *buflen,
                                          const char *inbuf,
                                          unsigned int inbuflen)
{
  DBUG_ENTER("caching_sha2_password_generate");
  std::string digest;
  std::string source(inbuf, inbuflen);
  std::string random;
  std::string serialized_string;

  if (my_validate_password_policy(inbuf, inbuflen))
    DBUG_RETURN(1);

  if (inbuflen == 0)
  {
    *buflen= 0;
    DBUG_RETURN(0);
  }

  char salt[sha2_password::SALT_LENGTH + 1];
  generate_user_salt(salt, sha2_password::SALT_LENGTH + 1);
  random.assign(salt, sha2_password::SALT_LENGTH);

  if (g_caching_sha2_password->generate_sha2_multi_hash(source, random,
        &digest, CACHING_SHA2_PASSWORD_ITERATIONS))
    DBUG_RETURN(1);

  if (g_caching_sha2_password->serialize(serialized_string,
                                         g_caching_sha2_password->get_digest_type(),
                                         random, digest,
                                         g_caching_sha2_password->get_digest_rounds()))
    DBUG_RETURN(1);

  if (serialized_string.length() > MAX_FIELD_WIDTH)
  {
    *buflen= 0;
    DBUG_RETURN(1);
  }
  memcpy(outbuf, serialized_string.c_str(), serialized_string.length());
  *buflen= serialized_string.length();

  DBUG_RETURN(0);
}


/**
  Validate a hash against caching_sha2_password plugin's
  hash format

  @param [in] inbuf  Hash to be validated
  @param [in] buflen Length of the hash

  @returns status of hash validation
    @retval 0 Hash is according to caching_sha2_password's expected format
    @retval 1 Hash does not match caching_sha2_password's requirement
*/

static int caching_sha2_password_validate(char* const inbuf,
                                          unsigned int buflen)
{
  DBUG_ENTER("caching_sha2_password_validate");
  std::string serialized_string(inbuf, buflen);
  if (g_caching_sha2_password->validate_hash(serialized_string))
    DBUG_RETURN(1);
  DBUG_RETURN(0);
}


/**
  NoOp - Salt generation for cachhing_sha2_password plugin.

  @param [in]  password     Unused
  @param [in]  password_len Unused
  @param [out] salt         Unused
  @param [out] salt_len     Length of the salt buffer

  @returns Always returns success (0)
*/

static int caching_sha2_password_salt(
  const char* password MY_ATTRIBUTE((unused)),
  unsigned int password_len MY_ATTRIBUTE((unused)),
  unsigned char* salt MY_ATTRIBUTE((unused)),
  unsigned char *salt_len)
{
  DBUG_ENTER("caching_sha2_password_salt");
  *salt_len= 0;
  DBUG_RETURN(0);
}


/*
  Initialize caching_sha2_password plugin

  @param [in] plugin_ref Plugin structure handle

  @returns Status of plugin initialization
    @retval 0 Success
    @retval 1 Error
*/

static int caching_sha2_authentication_init(MYSQL_PLUGIN plugin_ref)
{
  DBUG_ENTER("caching_sha2_authentication_init");
  caching_sha2_auth_plugin_ref= plugin_ref;
  g_caching_sha2_password=
    new sha2_password::Caching_sha2_password(caching_sha2_auth_plugin_ref);
  if (!g_caching_sha2_password)
    DBUG_RETURN(1);

  DBUG_RETURN(0);
}


/**
  Deinitialize caching_sha2_password plugin

  @param [in] arg Unused

  @returns Always returns success
*/

static int caching_sha2_authentication_deinit(void *arg MY_ATTRIBUTE((unused)))
{
  DBUG_ENTER("caching_sha2_authentication_deinit");
  if (g_caching_sha2_password)
  {
    delete g_caching_sha2_password;
    g_caching_sha2_password= 0;
  }
  DBUG_RETURN(0);
}


/**
  Compare a clear text password with a stored hash

  Check if stored hash is produced using a clear text password.
  To do that, first extra scrmable from the hash. Then
  calculate a new hash using extracted scramble and the supplied
  password. And finally compare the two hashes.

  @arg hash              pointer to the hashed data
  @arg hash_length       length of the hashed data
  @arg cleartext         pointer to the clear text password
  @arg cleartext_length  length of the cleat text password
  @arg[out] is_error     non-zero in case of error extracting the salt
  @retval 0              the hash was created with that password
  @retval non-zero       the hash was created with a different password
*/

static int
compare_caching_sha2_password_with_hash(
  const char *hash,
  unsigned long hash_length,
  const char *cleartext,
  unsigned long cleartext_length,
  int *is_error)
{
  DBUG_ENTER("compare_caching_sha2_password_with_hash");

  std::string serialized_string(hash, hash_length);
  std::string plaintext_password(cleartext, cleartext_length);
  std::string random;
  std::string digest;
  std::string generated_digest;
  sha2_password::Digest_info digest_type;
  size_t iterations;

  if (g_caching_sha2_password->deserialize(serialized_string,
        digest_type, random, digest, iterations))
  {
    *is_error= 1;
    DBUG_RETURN(-1);
  }

  if (g_caching_sha2_password->generate_sha2_multi_hash(
        plaintext_password, random, &generated_digest,
        iterations))
  {
    *is_error= 1;
    DBUG_RETURN(-1);
  }

  *is_error= 0;
  int result= memcmp(digest.c_str(), generated_digest.c_str(),
                     sha2_password::STORED_SHA256_DIGEST_LENGTH);

  DBUG_RETURN(result);
}


/** st_mysql_auth for caching_sha2_password plugin */
static struct st_mysql_auth caching_sha2_auth_handler
{
  MYSQL_AUTHENTICATION_INTERFACE_VERSION,
  caching_sha2_password_plugin_name.str,
  caching_sha2_password_authenticate,
  caching_sha2_password_generate,
  caching_sha2_password_validate,
  caching_sha2_password_salt,
  AUTH_FLAG_USES_INTERNAL_STORAGE,
  compare_caching_sha2_password_with_hash 
};


static MYSQL_SYSVAR_STR(private_key_path, caching_sha2_rsa_private_key_path,
  PLUGIN_VAR_READONLY | PLUGIN_VAR_NOPERSIST,
  "A fully qualified path to the private RSA key used for authentication.",
  NULL, NULL, AUTH_DEFAULT_RSA_PRIVATE_KEY);

static MYSQL_SYSVAR_STR(public_key_path, caching_sha2_rsa_public_key_path,
  PLUGIN_VAR_READONLY | PLUGIN_VAR_NOPERSIST,
  "A fully qualified path to the public RSA key used for authentication.",
  NULL, NULL, AUTH_DEFAULT_RSA_PUBLIC_KEY);

static struct st_mysql_sys_var* caching_sha2_password_sysvars[]= {
  MYSQL_SYSVAR(private_key_path),
  MYSQL_SYSVAR(public_key_path),
  0
};

/**
  Handle an authentication audit event.

  @param [in] thd         MySQL Thread Handle
  @param [in] event_class Event class information
  @param [in] event       Event structure

  @returns Success always.
*/

static int
sha2_cache_cleaner_notify(MYSQL_THD thd,
                          mysql_event_class_t event_class,
                          const void *event)
{
  DBUG_ENTER("sha2_cache_cleaner_notify");
  if (event_class == MYSQL_AUDIT_AUTHENTICATION_CLASS)
  {
    const struct mysql_event_authentication *authentication_event=
      (const struct mysql_event_authentication *) event;

    mysql_event_authentication_subclass_t subclass=
      authentication_event->event_subclass;

    /*
      If status is set to true, it indicates an error.
      In which case, don't touch the cache.
    */
    if (authentication_event->status)
      DBUG_RETURN(0);

    if (subclass == MYSQL_AUDIT_AUTHENTICATION_FLUSH)
    {
      g_caching_sha2_password->clear_cache();
      DBUG_RETURN(0);
    }

    if (subclass == MYSQL_AUDIT_AUTHENTICATION_CREDENTIAL_CHANGE ||
        subclass == MYSQL_AUDIT_AUTHENTICATION_AUTHID_RENAME ||
        subclass == MYSQL_AUDIT_AUTHENTICATION_AUTHID_DROP)
    {
      DBUG_ASSERT(authentication_event->user.str[authentication_event->user.length] == '\0');
      std::string authorization_id;
      make_hash_key(authentication_event->user.str,
                    authentication_event->host.str,
                    authorization_id);
      g_caching_sha2_password->remove_cached_entry(authorization_id);
    }
  }
  DBUG_RETURN(0);
}


/** st_mysql_audit for sha2_cache_cleaner plugin */
struct st_mysql_audit sha2_cache_cleaner=
{
  MYSQL_AUDIT_INTERFACE_VERSION,                    /* interface version */
  NULL,                                             /* release_thd() */
  sha2_cache_cleaner_notify,                        /* event_notify() */
  {0,                                               /* MYSQL_AUDIT_GENERAL_CLASS */
   0,                                               /* MYSQL_AUDIT_CONNECTION_CLASS */
   0,                                               /* MYSQL_AUDIT_PARSE_CLASS */
   0,                                               /* MYSQL_AUDIT_AUTHORIZATION_CLASS */
   0,                                               /* MYSQL_AUDIT_TABLE_ACCESS_CLASS */
   0,                                               /* MYSQL_AUDIT_GLOBAL_VARIABLE_CLASS */
   0,                                               /* MYSQL_AUDIT_SERVER_STARTUP_CLASS */
   0,                                               /* MYSQL_AUDIT_SERVER_SHUTDOWN_CLASS */
   0,                                               /* MYSQL_AUDIT_COMMAND_CLASS */
   0,                                               /* MYSQL_AUDIT_QUERY_CLASS */
   0,                                               /* MYSQL_AUDIT_STORED_PROGRAM_CLASS */
   (unsigned long) MYSQL_AUDIT_AUTHENTICATION_ALL   /* MYSQL_AUDIT_AUTHENTICATION_CLASS */
  }
};


/** Init function for sha2_cache_cleaner */
static int
caching_sha2_cache_cleaner_init(MYSQL_PLUGIN plugin_info MY_ATTRIBUTE((unused)))
{
  return 0;
}

/** Deinit function for sha2_cache_cleaner */
static int
caching_sha2_cache_cleaner_deinit(void *arg MY_ATTRIBUTE((unused)))
{
  return 0;
}


/*
  caching_sha2_password plugin declaration
*/

mysql_declare_plugin(caching_sha2_password)
{
  MYSQL_AUTHENTICATION_PLUGIN,                     /* plugin type                   */
  &caching_sha2_auth_handler,                      /* type specific descriptor      */
  caching_sha2_password_plugin_name.str,           /* plugin name                   */
  "Oracle",                                        /* author                        */
  "Caching sha2 authentication",                   /* description                   */
  PLUGIN_LICENSE_GPL,                              /* license                       */
  caching_sha2_authentication_init,                /* plugin initializer            */
  NULL,                                            /* Uninstall notifier            */
  caching_sha2_authentication_deinit,              /* plugin deinitializer          */
  0x0100,                                          /* version (1.0)                 */
  NULL,                                            /* status variables              */
  caching_sha2_password_sysvars,                   /* system variables              */
  NULL,                                            /* reserverd                     */
  0,                                               /* flags                         */
},
{
  MYSQL_AUDIT_PLUGIN,                              /* plugin type                   */
  &sha2_cache_cleaner,                             /* type specific descriptor      */
  "sha2_cache_cleaner",                            /* plugin name                   */
  "Oracle Inc",                                    /* author                        */
  "Cache cleaner for Caching sha2 authentication", /* description                   */
  PLUGIN_LICENSE_GPL,                              /* license                       */
  caching_sha2_cache_cleaner_init,                 /* plugin initializer            */
  NULL,                                            /* Uninstall notifier            */
  caching_sha2_cache_cleaner_deinit,               /* plugin deinitializer          */
  0x0100,                                          /* version (1.0)                 */
  NULL,                                            /* status variables              */
  NULL,                                            /* system variables              */
  NULL,                                            /* reserverd                     */
  0                                                /* flags                         */
}
mysql_declare_plugin_end;
