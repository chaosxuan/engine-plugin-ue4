// Copyright (C) 2013-2016 iFunFactory Inc. All Rights Reserved.
//
// This work is confidential and proprietary to iFunFactory Inc. and
// must not be used, disclosed, copied, or distributed without the prior
// consent of iFunFactory Inc.

#ifndef SRC_FUNAPI_ENCRYPTION_H_
#define SRC_FUNAPI_ENCRYPTION_H_

#include "funapi_build_config.h"

#include <memory>
#include <map>
#include <string>

namespace fun {

enum class FUNAPI_API EncryptionType : int {
  kNoneEncryption = 0,
  kDefaultEncryption = 100,
  kDummyEncryption,
  kIFunEngine1Encryption,
  kIFunEngine2Encryption,
  kChacha20Encryption,
  kAes128Encryption,
};

class FunapiEncryptionImpl;
class FUNAPI_API FunapiEncryption : public std::enable_shared_from_this<FunapiEncryption> {
 public:
  typedef std::map<std::string, std::string> HeaderFields;

  FunapiEncryption();
  virtual ~FunapiEncryption();

  void SetEncryptionType(EncryptionType type);
  void SetEncryptionType(EncryptionType type, const std::string &key);

  bool Encrypt(HeaderFields &header_fields, std::vector<uint8_t> &body);
  bool Decrypt(HeaderFields &header_fields, std::vector<uint8_t> &body);

  void SetHeaderFieldsForHttpSend (HeaderFields &header_fields);
  void SetHeaderFieldsForHttpRecv (HeaderFields &header_fields);

  bool UseSodium();

 private:
  std::shared_ptr<FunapiEncryptionImpl> impl_;
};

}  // namespace fun

#endif  // SRC_FUNAPI_ENCRYPTION_H_
