//
//  Created by Artem Brazhnikov on 11/15.
//  Copyright � 2015 Mantano. All rights reserved.
//
//  This program is distributed in the hope that it will be useful, but WITHOUT ANY
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
//
//  Licensed under Gnu Affero General Public License Version 3 (provided, notwithstanding this notice,
//  Readium Foundation reserves the right to license this material under a different separate license,
//  and if you have done so, the terms of that separate license control and the following references
//  to GPL do not apply).
//
//  This program is free software: you can redistribute it and/or modify it under the terms of the GNU
//  Affero General Public License as published by the Free Software Foundation, either version 3 of
//  the License, or (at your option) any later version. You should have received a copy of the GNU
//  Affero General Public License along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <functional>
#include <chrono>
#include "CryptoppCryptoProvider.h"
#include "CryptoAlgorithmInterfaces.h"
#include "EncryptionProfilesManager.h"
#include "Public/ILicense.h"
#include "Public/ICrypto.h"
#include "Certificate.h"
#include "CertificateRevocationList.h"
#include "CrlUpdater.h"
#include "ThreadTimer.h"
#include "DateTime.h"
#include "IKeyProvider.h"
#include "CryptoppUtils.h"
#include "Sha256HashAlgorithm.h"
#include "SymmetricAlgorithmEncryptedStream.h"

namespace lcp
{
    CryptoppCryptoProvider::CryptoppCryptoProvider(
        EncryptionProfilesManager * encryptionProfilesManager,
        INetProvider * netProvider
        )
        : m_encryptionProfilesManager(encryptionProfilesManager)
    {
        m_revocationList.reset(new CertificateRevocationList());
        m_threadTimer.reset(new ThreadTimer());
        m_crlUpdater.reset(new CrlUpdater(netProvider, m_revocationList.get(), m_threadTimer.get()));

        m_threadTimer->SetHandler(std::bind(&CrlUpdater::Update, m_crlUpdater.get()));
        m_threadTimer->SetAutoReset(true);
    }

    CryptoppCryptoProvider::~CryptoppCryptoProvider()
    {
        m_crlUpdater->Cancel();
        m_threadTimer->Stop();
    }

    Status CryptoppCryptoProvider::VerifyLicense(
        const std::string & rootCertificateBase64,
        ILicense * license
        )
    {
        try
        {
            IEncryptionProfile * profile = m_encryptionProfilesManager->GetProfile(license->Crypto()->EncryptionProfile());
            if (profile == nullptr)
            {
                return Status(StatusCode::ErrorCommonEncryptionProfileNotFound);
            }

            if (rootCertificateBase64.empty())
            {
                return Status(StatusCode::ErrorOpeningNoRootCertificate);
            }

            std::unique_ptr<Certificate> rootCertificate;
            try
            {
                rootCertificate.reset(new Certificate(rootCertificateBase64, profile));
            }
            catch (CryptoPP::BERDecodeErr & ex)
            {
                return Status(StatusCode::ErrorOpeningRootCertificateNotValid, ex.GetWhat());
            }

            std::unique_ptr<Certificate> providerCertificate;
            try
            {
                providerCertificate.reset(new Certificate(license->Crypto()->SignatureCertificate(), profile));
            }
            catch (CryptoPP::BERDecodeErr & ex)
            {
                return Status(StatusCode::ErrorOpeningContentProviderCertificateNotValid, ex.GetWhat());
            }

            if (!providerCertificate->VerifyCertificate(rootCertificate.get()))
            {
                return Status(StatusCode::ErrorOpeningContentProviderCertificateNotVerified);
            }

            Status res = this->ProcessRevokation(providerCertificate.get());
            if (!Status::IsSuccess(res))
            {
                return res;
            }

            if (!providerCertificate->VerifyMessage(license->CanonicalContent(), license->Crypto()->Signature()))
            {
                return Status(StatusCode::ErrorOpeningLicenseSignatureNotValid);
            }

            DateTime notBefore(providerCertificate->NotBeforeDate());
            DateTime notAfter(providerCertificate->NotAfterDate());

            DateTime lastUpdated;
            if (!license->Updated().empty())
            {
                lastUpdated = DateTime(license->Updated());
            }
            else
            {
                lastUpdated = DateTime(license->Issued());
            }

            if (lastUpdated < notBefore)
            {
                return Status(StatusCode::ErrorOpeningContentProviderCertificateNotStarted);
            }
            else if (lastUpdated > notAfter)
            {
                return Status(StatusCode::ErrorOpeningContentProviderCertificateExpired);
            }
            return Status(StatusCode::ErrorCommonSuccess);
        }
        catch (const CryptoPP::Exception & ex)
        {
            return Status(StatusCode::ErrorOpeningContentProviderCertificateNotVerified, ex.GetWhat());
        }
    }

    Status CryptoppCryptoProvider::DecryptUserKey(
        const std::string & userPassphrase,
        ILicense * license,
        KeyType & userKey
        )
    {
        try
        {
            IEncryptionProfile * profile = m_encryptionProfilesManager->GetProfile(license->Crypto()->EncryptionProfile());
            if (profile == nullptr)
            {
                return Status(StatusCode::ErrorCommonEncryptionProfileNotFound);
            }

            std::unique_ptr<IHashAlgorithm> hashAlgorithm(profile->CreateUserKeyAlgorithm());
            hashAlgorithm->UpdateHash(userPassphrase);
            userKey = hashAlgorithm->Hash();

            std::unique_ptr<ISymmetricAlgorithm> contentKeyAlgorithm(profile->CreateContentKeyAlgorithm(userKey));
            std::string id = contentKeyAlgorithm->Decrypt(license->Crypto()->UserKeyCheck());
            if (!EqualsUtf8(id, license->Id()))
            {
                return Status(StatusCode::ErrorDecryptionUserPassphraseNotValid);
            }
            return Status(StatusCode::ErrorCommonSuccess);
        }
        catch (const CryptoPP::Exception & ex)
        {
            return Status(StatusCode::ErrorDecryptionUserPassphraseNotValid, ex.GetWhat());
        }
    }

    Status CryptoppCryptoProvider::DecryptContentKey(
        const KeyType & userKey,
        ILicense * license,
        KeyType & contentKey
        )
    {
        try
        {
            IEncryptionProfile * profile = m_encryptionProfilesManager->GetProfile(license->Crypto()->EncryptionProfile());
            if (profile == nullptr)
            {
                return Status(StatusCode::ErrorCommonEncryptionProfileNotFound);
            }

            std::unique_ptr<ISymmetricAlgorithm> contentKeyAlgorithm(profile->CreateContentKeyAlgorithm(userKey));
            std::string decryptedContentKey = contentKeyAlgorithm->Decrypt(license->Crypto()->ContentKey());

            contentKey.assign(decryptedContentKey.begin(), decryptedContentKey.end());
            return Status(StatusCode::ErrorCommonSuccess);
        }
        catch (const CryptoPP::Exception & ex)
        {
            return Status(StatusCode::ErrorDecryptionLicenseEncrypted, ex.GetWhat());
        }
    }

    Status CryptoppCryptoProvider::CalculateFileHash(
        IReadableStream * readableStream,
        std::vector<unsigned char> & rawHash
        )
    {
        try
        {
            Sha256HashAlgorithm algorithm;
            size_t bufferSize = 1024 * 1024;
            std::vector<unsigned char> buffer(bufferSize);
            
            size_t read = 0;
            size_t sizeToRead = bufferSize;
            size_t fileSize = static_cast<size_t>(readableStream->Size());
            while (read != fileSize)
            {
                sizeToRead = (fileSize - read > bufferSize) ? bufferSize : fileSize - read;
                readableStream->Read(buffer.data(), sizeToRead);
                algorithm.UpdateHash(buffer.data(), sizeToRead);
                read += sizeToRead;
            }
            rawHash = algorithm.Hash();

            return Status(StatusCode::ErrorCommonSuccess);
        }
        catch (const CryptoPP::Exception & ex)
        {
            return Status(StatusCode::ErrorDecryptionCommonError, ex.GetWhat());
        }
    }

    Status CryptoppCryptoProvider::ConvertRawToHex(
        const std::vector<unsigned char> & data,
        std::string & hex
        )
    {
        try
        {
            hex = CryptoppUtils::RawToHex(data);
            return Status(StatusCode::ErrorCommonSuccess);
        }
        catch (const CryptoPP::Exception & ex)
        {
            return Status(StatusCode::ErrorDecryptionCommonError, ex.GetWhat());
        }
    }

    Status CryptoppCryptoProvider::ConvertHexToRaw(
        const std::string & hex,
        std::vector<unsigned char> & data
        )
    {
        try
        {
            data = CryptoppUtils::HexToRaw(hex);
            return Status(StatusCode::ErrorCommonSuccess);
        }
        catch (const CryptoPP::Exception & ex)
        {
            return Status(StatusCode::ErrorDecryptionCommonError, ex.GetWhat());
        }
    }

    Status CryptoppCryptoProvider::DecryptLicenseData(
        const std::string & dataBase64,
        ILicense * license,
        IKeyProvider * keyProvider,
        std::string & decrypted
        )
    {
        try
        {
            IEncryptionProfile * profile = m_encryptionProfilesManager->GetProfile(license->Crypto()->EncryptionProfile());
            if (profile == nullptr)
            {
                return Status(StatusCode::ErrorCommonEncryptionProfileNotFound);
            }

            std::unique_ptr<ISymmetricAlgorithm> contentKeyAlgorithm(profile->CreateContentKeyAlgorithm(keyProvider->UserKey()));
            decrypted = contentKeyAlgorithm->Decrypt(dataBase64);
            return Status(StatusCode::ErrorCommonSuccess);
        }
        catch (const CryptoPP::Exception & ex)
        {
            return Status(StatusCode::ErrorDecryptionLicenseEncrypted, ex.GetWhat());
        }
    }

    Status CryptoppCryptoProvider::DecryptPublicationData(
        ILicense * license,
        IKeyProvider * keyProvider,
        const unsigned char * data,
        const size_t dataLength,
        unsigned char * decryptedData,
        size_t inDecryptedDataLength,
        size_t * outDecryptedDataLength
        )
    {
        try
        {
            IEncryptionProfile * profile = m_encryptionProfilesManager->GetProfile(license->Crypto()->EncryptionProfile());
            if (profile == nullptr)
            {
                return Status(StatusCode::ErrorCommonEncryptionProfileNotFound);
            }

            std::unique_ptr<ISymmetricAlgorithm> algorithm(profile->CreatePublicationAlgorithm(keyProvider->ContentKey()));
            *outDecryptedDataLength = algorithm->Decrypt(
                data, dataLength, decryptedData, inDecryptedDataLength
                );

            return Status(StatusCode::ErrorCommonSuccess);
        }
        catch (const CryptoPP::Exception & ex)
        {
            return Status(StatusCode::ErrorDecryptionPublicationEncrypted, ex.GetWhat());
        }
    }

    Status CryptoppCryptoProvider::CreateEncryptedPublicationStream(
        ILicense * license,
        IKeyProvider * keyProvider,
        IReadableStream * stream,
        IEncryptedStream ** encStream
        )
    {
        try
        {
            IEncryptionProfile * profile = m_encryptionProfilesManager->GetProfile(license->Crypto()->EncryptionProfile());
            if (profile == nullptr)
            {
                return Status(StatusCode::ErrorCommonEncryptionProfileNotFound);
            }

            Status res(StatusCode::ErrorCommonSuccess);
            std::unique_ptr<ISymmetricAlgorithm> algorithm(profile->CreatePublicationAlgorithm(keyProvider->ContentKey()));
            *encStream = new SymmetricAlgorithmEncryptedStream(stream, std::move(algorithm));
            return res;
        }
        catch (const CryptoPP::Exception & ex)
        {
            return Status(StatusCode::ErrorDecryptionPublicationEncrypted, ex.GetWhat());
        }
    }

    Status CryptoppCryptoProvider::ProcessRevokation(ICertificate * providerCertificate)
    {
        bool containedAnyUrlBefore = m_crlUpdater->ContainsAnyUrl();
        m_crlUpdater->UpdateCrlDistributionPoints(providerCertificate->DistributionPoints());

        // First time processing of the CRL
        if (!containedAnyUrlBefore && m_crlUpdater->ContainsAnyUrl())
        {
            m_crlUpdater->Update();
            // Start timer which checks CRL for updates periodically or by time point
            m_threadTimer->Start();
        }

        // If exception occurred in the timer thread, re-throw it
        m_threadTimer->RethrowExceptionIfAny();

        if (m_revocationList->SerialNumberRevoked(providerCertificate->SerialNumber()))
        {
            return Status(StatusCode::ErrorOpeningContentProviderCertificateRevoked);
        }
        return Status(StatusCode::ErrorCommonSuccess);
    }
}