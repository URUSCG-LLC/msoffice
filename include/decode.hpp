#pragma once
/**
	@file
	@brief MS Office encryption decoder
	Copyright (C) 2012 Cybozu Labs, Inc., all rights reserved.
	see [MS-OFFCRYPTO]
	Office Document Cryptography Structure Specification
*/
#include <fstream>
#include <cybozu/mmap.hpp>
#include <cybozu/minixml.hpp>
#include <cybozu/atoi.hpp>
#include <cybozu/crypto.hpp>
#include <cybozu/random_generator.hpp>
#include "cfb.hpp"
#include "crypto_util.hpp"

namespace ms {

inline void DecContent(std::string& dec, const std::string& data, const CipherParam& param, const std::string& key, const std::string& salt)
{
	const size_t blockSize = 4096;
	dec.reserve(data.size());
	const size_t n = (data.size() + blockSize - 1) / blockSize;
	for (size_t i = 0; i < n; i++) {
		const size_t len = (i < n - 1) ? blockSize : (data.size() % blockSize);
		std::string blockKey(4, 0);
		cybozu::Set32bitAsLE(&blockKey[0], static_cast<uint32_t>(i));
		const std::string iv = generateKey(param, salt, blockKey);
		dec.append(cipher(param.cipherName, data.c_str() + i * blockSize, len, key, iv, cybozu::crypto::Cipher::Decoding));
	}
}

/*
	split encryptedPackage as [uint64_t:encData]
*/
inline uint64_t GetEncodedData(std::string& encData, const std::string& encryptedPackage)
{
	if (encryptedPackage.size() < 8) {
		throw cybozu::Exception("ms:GetEncodedData:tool small") << encryptedPackage.size();
	}
	const char *p = &encryptedPackage[0];
	const uint64_t size = cybozu::Get64bitAsLE(p);
	dprintf("package size:header %d encryptedPackage %d\n", (int)size, (int)encryptedPackage.size());
	MS_ASSERT(encryptedPackage.size() - 8 >= size);
	encData = encryptedPackage.substr(8);
	return size;
}

inline const std::string& GetContensByName(const ms::cfb::CompoundFile& cfb, const std::string& name)
{
	const cybozu::String16 wname = cybozu::ToUtf16(name);
	const ms::cfb::DirectoryEntryVec& dirs = cfb.dirs;
	for (size_t i = 0; i < dirs.size(); i++) {
		const ms::cfb::DirectoryEntry& dir = dirs[i];
		if (dir.directoryEntryName == wname) {
			return dir.content;
		}
	}
	throw cybozu::Exception("ms:GetContentsByName:name") << name;
}

/*
	verify integrity
	hmac = openssl dgst -sha1 -mac HMAC -macopt hexkey:hex(salt) encryptedpackage
	hmac == hex(expected)
*/
inline bool VerifyIntegrity(
	const std::string& encryptedPackage,
	const CipherParam& keyData,
	const std::string& secretKey,
	const std::string& saltValue,
	const std::string& encryptedHmacKey,
	const std::string& encryptedHmacValue)
{
	const std::string iv1 = generateIv(keyData, ms::blkKey_dataIntegrity1, saltValue);
	const std::string iv2 = generateIv(keyData, ms::blkKey_dataIntegrity2, saltValue);
	const std::string salt = cipher(keyData.cipherName, encryptedHmacKey, secretKey, iv1, cybozu::crypto::Cipher::Decoding).substr(0, keyData.hashSize);
	const std::string expected = cipher(keyData.cipherName, encryptedHmacValue, secretKey, iv2, cybozu::crypto::Cipher::Decoding).substr(0, keyData.hashSize);

	cybozu::crypto::Hmac hmac(keyData.hashName);
	std::string ret = hmac.eval(salt, encryptedPackage);
	return ret == expected;
}

inline bool decodeAgile(std::string& decData, const std::string& encryptedPackage, const EncryptionInfo& info, const std::string& pass, const std::string& masterKey)
{
	const CipherParam& keyData = info.keyData;
	const CipherParam& encryptedKey = info.encryptedKey;
	const std::string& iv = encryptedKey.saltValue;

	const std::string pwHash = hashPassword(encryptedKey.hashName, iv, pass, info.spinCount);
	const std::string skey1 = generateKey(encryptedKey, pwHash, ms::blkKey_VerifierHashInput);
	const std::string skey2 = generateKey(encryptedKey, pwHash, ms::blkKey_encryptedVerifierHashValue);

	const std::string verifierHashInput = cipher(encryptedKey.cipherName, info.encryptedVerifierHashInput, skey1, iv, cybozu::crypto::Cipher::Decoding);
	const std::string hashedVerifier = cybozu::crypto::Hash::digest(encryptedKey.hashName, verifierHashInput);
	const std::string verifierHash = cipher(encryptedKey.cipherName, info.encryptedVerifierHashValue, skey2, iv, cybozu::crypto::Cipher::Decoding).substr(0, hashedVerifier.size());

	if (hashedVerifier != verifierHash) {
		if (masterKey.empty()) return false;
	}
	const std::string skey3 = generateKey(encryptedKey, pwHash, ms::blkKey_encryptedKeyValue);
	std::string secretKey = cipher(encryptedKey.cipherName, info.encryptedKeyValue, skey3, iv, cybozu::crypto::Cipher::Decoding);
	if (isDebug()) {
		printf("salt="); dump(keyData.saltValue, false);
		printf("secretKey="); dump(secretKey, false);
	}

	if (masterKey.empty()) {
		if (!VerifyIntegrity(encryptedPackage, keyData, secretKey, keyData.saltValue, info.encryptedHmacKey, info.encryptedHmacValue)) {
			printf("warning : mac err : data may be broken\n");
//			return false;
		}
	} else {
		secretKey = masterKey;
	}

	std::string encData;
	const uint64_t decodeSize = GetEncodedData(encData, encryptedPackage);

	// decode
	DecContent(decData, encData, encryptedKey, secretKey, keyData.saltValue);
	decData.resize(decodeSize);
	return true;
}

/*
	2.3.4.9
*/
inline bool verifyStandardEncryption(std::string& encKey, const EncryptionHeader& header, const EncryptionVerifier& verifier, const std::string& pass)
{
	const cybozu::crypto::Hash::Name hashName = cybozu::crypto::Hash::N_SHA1;
	encKey = verifier.getEncryptionKey(pass).substr(0, header.keySize / 8);
	const std::string iv;
	const std::string decVerifier = cipher(header.cipherName, verifier.encryptedVerifier, encKey, iv, cybozu::crypto::Cipher::Decoding);
	const std::string h = cybozu::crypto::Hash::digest(hashName, decVerifier);
	std::string decVerifierHash = cipher(header.cipherName, verifier.encryptedVerifierHash, encKey, iv, cybozu::crypto::Cipher::Decoding);
	decVerifierHash.resize(h.size());
	return h == decVerifierHash;
}

inline bool decodeStandardEncryption(std::string& dec, const std::string& encryptedPackage, const EncryptionInfo& info, const std::string& pass, const std::string& masterKey)
{
	cybozu::disable_warning_unused_variable(masterKey);

	const EncryptionHeader& header = info.seHeader;
	const EncryptionVerifier& verifier = info.seVerifier;

	std::string secretKey;
	if (masterKey.empty()) {
		if (!verifyStandardEncryption(secretKey, header, verifier, pass)) {
			return false;
		}
	} else {
		secretKey = masterKey;
	}
	if (isDebug()) {
		printf("secretKey="); dump(secretKey, false);
	}

	const char *p = encryptedPackage.data();
	size_t decSize = cybozu::Get32bitAsLE(p);
	p += 8;
	const size_t dataSize = encryptedPackage.size();
	if (decSize > dataSize) {
		throw cybozu::Exception("ms:decodeStandardEncryption:bad decSize") << decSize << dataSize;
	}
	const size_t blockSize = 4096;
	dec.reserve(dataSize);
	const size_t n = (dataSize + blockSize - 1) / blockSize;
	const std::string iv;
	for (size_t i = 0; i < n; i++) {
		const size_t len = (i < n - 1) ? blockSize : (dataSize % blockSize);
		dec.append(cipher(header.cipherName, p + i * blockSize, len, secretKey, iv, cybozu::crypto::Cipher::Decoding));
	}
	dec.resize(decSize);
	return true;
}

inline bool decode(const char *data, uint32_t dataSize, const std::string& outFile, const std::string& pass, const std::string& masterKey, bool doView)
{
	ms::cfb::CompoundFile cfb(data, dataSize);
	cfb.put();

	const std::string& encryptedPackage = GetContensByName(cfb, "EncryptedPackage"); // data
	const EncryptionInfo info(GetContensByName(cfb, "EncryptionInfo")); // xml

	std::string decData;
	if (info.isStandardEncryption) {
		if (!decodeStandardEncryption(decData, encryptedPackage, info, pass, masterKey)) return false;
	} else {
		if (!decodeAgile(decData, encryptedPackage, info, pass, masterKey)) return false;
	}
	if (!doView) {
		std::ofstream ofs(outFile.c_str(), std::ios::binary);
		ofs.write(decData.c_str(), decData.size());
		if (!ofs) throw cybozu::Exception("ms:decode:save") << outFile;
	}
	return true;
}

} // ms
