
#include <openssl/ec.h>
#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/pem.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <vector>
#include <cassert>

#include "key.h"

#define ECIES_KEY_HASH		SHA256
#define ECIES_KEY_LENGTH	(256/8)
#define ECIES_KEY_TYPE		uint256
#define ECIES_ENC_ALGO		EVP_aes_256_cbc()
#define ECIES_ENC_KEY_SIZE 	(256/8)
#define ECIES_ENC_BLK_SIZE	(128/8)
#define ECIES_ENC_KEY_TYPE	uint256
#define ECIES_ENC_IV_TYPE	uint128
#define ECIES_HMAC_ALGO		EVP_sha256()
#define ECIES_HMAC_SIZE		(256/8)
#define ECIES_HMAC_TYPE		uint256

static void* ecies_key_derivation(const void *input, size_t ilen, void *output, size_t *olen)
{ // This function must not be changed as it must be what ECDH_compute_key expects
	if (*olen < ECIES_KEY_LENGTH)
	{
		assert(false);
		return NULL;
	}
	*olen = ECIES_KEY_LENGTH;
	return ECIES_KEY_HASH(static_cast<const unsigned char *>(input), ilen, static_cast<unsigned char *>(output));
}

ECIES_KEY_TYPE CKey::getECIESSecret(CKey& otherKey)
{ // Retrieve a secret generated from an EC key pair. At least one private key must be known.
	if(!pkey || !otherKey.pkey)
		throw std::runtime_error("missing key");

	EC_KEY *pubkey, *privkey;
	if(EC_KEY_get0_private_key(pkey))
	{
		privkey=pkey;
		pubkey=otherKey.pkey;
	}
	else if(EC_KEY_get0_private_key(otherKey.pkey))
	{
		privkey=otherKey.pkey;
		pubkey=pkey;
	}
	else throw std::runtime_error("no private key");

	ECIES_KEY_TYPE key;
	if (ECDH_compute_key(key.begin(), ECIES_KEY_LENGTH, EC_KEY_get0_public_key(pubkey),
			privkey, ecies_key_derivation) != ECIES_KEY_LENGTH)
		throw std::runtime_error("ecdh key failed");
	return key;
}

// Our ciphertext is all encrypted except the IV. The encrypted data decodes as follows:
// 1) IV (unencrypted)
// 2) Encrypted: HMAC of original plaintext
// 3) Encrypted: Original plaintext
// 4) Encrypted: Rest of block/padding

static ECIES_HMAC_TYPE makeHMAC(ECIES_KEY_TYPE secret, const std::vector<unsigned char> data)
{
	HMAC_CTX ctx;
	HMAC_CTX_init(&ctx);
	
	if(HMAC_Init_ex(&ctx, secret.begin(), ECIES_KEY_LENGTH, ECIES_HMAC_ALGO, NULL) != 1)
	{
		HMAC_CTX_cleanup(&ctx);
		throw std::runtime_error("init hmac");
	}

	if(HMAC_Update(&ctx, &(data.front()), data.size()) != 1)
	{
		HMAC_CTX_cleanup(&ctx);
		throw std::runtime_error("update hmac");
	}

	unsigned int ml=EVP_MAX_MD_SIZE;
	std::vector<unsigned char> hmac(ml);
	if(HMAC_Final(&ctx, &(hmac.front()), &ml) != 1)
	{
		HMAC_CTX_cleanup(&ctx);
		throw std::runtime_error("finalize hmac");
	}

	ECIES_HMAC_TYPE ret;
	memcpy(ret.begin(), &(hmac.front()), ECIES_HMAC_SIZE);

	return ret;
}

std::vector<unsigned char> CKey::encryptECIES(CKey& otherKey, const std::vector<unsigned char>& plaintext)
{
	ECIES_KEY_TYPE secret=getECIESSecret(otherKey);
	ECIES_HMAC_TYPE hmac=makeHMAC(secret, plaintext);

	ECIES_ENC_IV_TYPE iv;
	if(RAND_bytes(static_cast<unsigned char *>(iv.begin()), ECIES_ENC_BLK_SIZE) != 1)
		throw std::runtime_error("insufficient entropy");

	EVP_CIPHER_CTX ctx;
	EVP_CIPHER_CTX_init(&ctx);

	if (EVP_EncryptInit_ex(&ctx, ECIES_ENC_ALGO, NULL, secret.begin(), iv.begin()) != 1)
	{
		EVP_CIPHER_CTX_cleanup(&ctx);
		throw std::runtime_error("init cipher ctx");
	}

	std::vector<unsigned char> out(plaintext.size() + ECIES_HMAC_SIZE + ECIES_ENC_KEY_SIZE + ECIES_ENC_BLK_SIZE, 0);
	int len=0, bytesWritten;

	// output IV
	memcpy(&(out.front()), iv.begin(), ECIES_ENC_BLK_SIZE);
	len=ECIES_ENC_BLK_SIZE;

	// Encrypt/output HMAC
	bytesWritten=out.capacity()-len;
	assert(bytesWritten>0);
	if(EVP_EncryptUpdate(&ctx, &(out.front()) + len, &bytesWritten, hmac.begin(), ECIES_HMAC_SIZE) < 0)
	{
		EVP_CIPHER_CTX_cleanup(&ctx);
		throw std::runtime_error("");
	}
	len+=bytesWritten;

	// encrypt/output plaintext
	bytesWritten=out.capacity()-len;
	assert(bytesWritten>0);
	if(EVP_EncryptUpdate(&ctx, &(out.front()) + len, &bytesWritten, &(plaintext.front()), plaintext.size()) < 0)
	{
		EVP_CIPHER_CTX_cleanup(&ctx);
		throw std::runtime_error("");
	}
	len+=bytesWritten;

	// finalize
	bytesWritten=out.capacity()-len;
	if(EVP_EncryptFinal_ex(&ctx, &(out.front()) + len, &bytesWritten) < 0)
	{
		EVP_CIPHER_CTX_cleanup(&ctx);
		throw std::runtime_error("");
	}
	len+=bytesWritten;

	// Output contains: IV, encrypted HMAC, encrypted data, encrypted padding
	assert(len <= (plaintext.size() + ECIES_HMAC_SIZE + (2 * ECIES_ENC_BLK_SIZE)));
	assert(len >= (plaintext.size() + ECIES_HMAC_SIZE + ECIES_ENC_BLK_SIZE)); // IV, HMAC, data
	out.resize(len);
	EVP_CIPHER_CTX_cleanup(&ctx);
	return out;
}

std::vector<unsigned char> CKey::decryptECIES(CKey& otherKey, const std::vector<unsigned char>& ciphertext)
{
	ECIES_KEY_TYPE secret=getECIESSecret(otherKey);

	// minimum ciphertext = IV + HMAC + 1 block
	if(ciphertext.size() < ((2*ECIES_ENC_BLK_SIZE) + ECIES_HMAC_SIZE) )
		throw std::runtime_error("ciphertext too short");

	// extract IV
	ECIES_ENC_IV_TYPE iv;
	memcpy(iv.begin(), &(ciphertext.front()), ECIES_ENC_BLK_SIZE);

	// begin decrypting
	EVP_CIPHER_CTX ctx;
	EVP_CIPHER_CTX_init(&ctx);

	if(EVP_DecryptInit_ex(&ctx, ECIES_ENC_ALGO, NULL, secret.begin(), iv.begin()) != 1)
	{
		EVP_CIPHER_CTX_cleanup(&ctx);
		throw std::runtime_error("unable to init cipher");
	}
	
	// decrypt mac
	ECIES_HMAC_TYPE hmac;
	int outlen=ECIES_HMAC_SIZE;
	if( (EVP_DecryptUpdate(&ctx, hmac.begin(), &outlen,
		&(ciphertext.front()) + ECIES_ENC_BLK_SIZE,	ECIES_HMAC_SIZE+1) != 1) || (outlen != ECIES_HMAC_SIZE) )
	{
		EVP_CIPHER_CTX_cleanup(&ctx);
		throw std::runtime_error("unable to extract hmac");
	}
	
	// decrypt plaintext (after IV and encrypted mac)
	std::vector<unsigned char> plaintext(ciphertext.size() - ECIES_HMAC_SIZE - ECIES_ENC_BLK_SIZE);
	outlen=plaintext.size();
	if(EVP_DecryptUpdate(&ctx, &(plaintext.front()), &outlen,
		&(ciphertext.front())+ECIES_ENC_BLK_SIZE+ECIES_HMAC_SIZE+1,
		ciphertext.size()-ECIES_ENC_BLK_SIZE-ECIES_HMAC_SIZE-1) != 1)
	{
		EVP_CIPHER_CTX_cleanup(&ctx);
		throw std::runtime_error("unable to extract plaintext");
	}

	int flen = 0;
	if(EVP_DecryptFinal(&ctx, &(plaintext.front()) + outlen, &flen) != 1)
	{
		EVP_CIPHER_CTX_cleanup(&ctx);
		throw std::runtime_error("plaintext had bad padding");
	}
	plaintext.resize(flen + outlen);

	if(hmac != makeHMAC(secret, plaintext))
	{
		EVP_CIPHER_CTX_cleanup(&ctx);
		throw std::runtime_error("plaintext had bad hmac");
	}

	EVP_CIPHER_CTX_cleanup(&ctx);
	return plaintext;
}

bool checkECIES(void)
{
	CKey senderPriv, recipientPriv, senderPub, recipientPub;
	senderPriv.MakeNewKey();
	recipientPriv.MakeNewKey();

	if(!senderPub.SetPubKey(senderPriv.GetPubKey()))
		throw std::runtime_error("key error");
	if(!recipientPub.SetPubKey(recipientPriv.GetPubKey()))
		throw std::runtime_error("key error");

	for(int i=0; i<30000; i++)
	{
		// generate message
		std::vector<unsigned char> message(4096);
		int msglen=i%3000;
		if(RAND_bytes(static_cast<unsigned char *>(&message.front()), msglen) != 1)
			throw std::runtime_error("insufficient entropy");
		message.resize(msglen);

		// encrypt message with sender's private key and recipient's public key
		std::vector<unsigned char> ciphertext=senderPriv.encryptECIES(recipientPub, message);

		// decrypt message with recipient's private key and sender's public key
		std::vector<unsigned char> decrypt=recipientPriv.decryptECIES(senderPub, ciphertext);

		if(decrypt != message) return false;
//		std::cerr << "Msg(" << msglen << ") ok " << ciphertext.size() << std::endl;
	}
	return true;
}

// vim:ts=4
