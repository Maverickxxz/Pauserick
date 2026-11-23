/*
 * pksign - signs Pauserick releases, so the built-in updater accepts them.
 *
 *   pksign keygen <private.key> <update_key.h>     create the key pair (never overwrites a key)
 *   pksign sign   <private.key> <version> <Pauserick.exe> <Pauserick.exe.sig>
 *   pksign verify <private.key> <version> <Pauserick.exe> <Pauserick.exe.sig>
 *
 * Signature: ECDSA P-256 over SHA-256("PAUSERICK-UPDATE\n" + version + "\n" + exe bytes),
 * with Windows' own crypto (bcrypt.dll). Binding the version stops an old signed exe from
 * being passed off as a newer one. The public half is compiled into Pauserick
 * (src/update_key.h); the private half must stay on the maintainer's PC: never commit it,
 * and back it up, because without it no update can ever be published again.
 *
 * Build: gcc -O2 -o build\pksign.exe tools\pksign.c -lbcrypt
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NT_OK(s) ((NTSTATUS)(s) >= 0)

static unsigned char *read_file(const char *path, DWORD *len)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(f, NULL), got = 0;
    unsigned char *buf = size != INVALID_FILE_SIZE ? malloc(size + 1) : NULL;
    if (buf && (!ReadFile(f, buf, size, &got, NULL) || got != size)) { free(buf); buf = NULL; }
    CloseHandle(f);
    if (buf) { buf[size] = 0; *len = size; }
    return buf;
}

static int write_file(const char *path, const void *data, DWORD len, DWORD disposition)
{
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, disposition, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0;
    BOOL ok = WriteFile(f, data, len, &w, NULL) && w == len;
    CloseHandle(f);
    return ok;
}

static int digest_of(const char *version, const unsigned char *data, DWORD len, unsigned char out[32])
{
    static const char prefix[] = "PAUSERICK-UPDATE\n";
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    int ok = NT_OK(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)) &&
             NT_OK(BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0)) &&
             NT_OK(BCryptHashData(h, (PUCHAR)prefix, sizeof prefix - 1, 0)) &&
             NT_OK(BCryptHashData(h, (PUCHAR)version, (ULONG)strlen(version), 0)) &&
             NT_OK(BCryptHashData(h, (PUCHAR)"\n", 1, 0)) &&
             NT_OK(BCryptHashData(h, (PUCHAR)data, len, 0)) &&
             NT_OK(BCryptFinishHash(h, out, 32, 0));
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static BCRYPT_KEY_HANDLE load_private(const char *path, BCRYPT_ALG_HANDLE *alg)
{
    DWORD len = 0;
    unsigned char *blob = read_file(path, &len);
    BCRYPT_KEY_HANDLE key = NULL;
    if (!blob) { fprintf(stderr, "cannot read the private key %s\n", path); return NULL; }
    if (!NT_OK(BCryptOpenAlgorithmProvider(alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0)) ||
        !NT_OK(BCryptImportKeyPair(*alg, NULL, BCRYPT_ECCPRIVATE_BLOB, &key, blob, len, 0))) {
        fprintf(stderr, "%s is not a P-256 private key\n", path);
        key = NULL;
    }
    SecureZeroMemory(blob, len);
    free(blob);
    return key;
}

static int keygen(const char *privPath, const char *headerPath)
{
    if (GetFileAttributesA(privPath) != INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "%s already exists: a signing key is never overwritten\n", privPath);
        return 1;
    }
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    unsigned char priv[256], pub[256];
    ULONG privLen = 0, pubLen = 0;
    int ok = NT_OK(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0)) &&
             NT_OK(BCryptGenerateKeyPair(alg, &key, 256, 0)) && NT_OK(BCryptFinalizeKeyPair(key, 0)) &&
             NT_OK(BCryptExportKey(key, NULL, BCRYPT_ECCPRIVATE_BLOB, priv, sizeof priv, &privLen, 0)) &&
             NT_OK(BCryptExportKey(key, NULL, BCRYPT_ECCPUBLIC_BLOB, pub, sizeof pub, &pubLen, 0));
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) { fprintf(stderr, "key generation failed\n"); return 1; }
    if (!write_file(privPath, priv, privLen, CREATE_NEW)) { fprintf(stderr, "cannot write %s\n", privPath); return 1; }
    SecureZeroMemory(priv, sizeof priv);

    char h[2048];
    int n = sprintf(h, "/* Public half of the Pauserick release signing key (ECDSA P-256, BCRYPT_ECCPUBLIC_BLOB),\n"
                       " * made by tools/pksign.c. The updater installs only files signed with the private half,\n"
                       " * which the maintainer keeps off the repository. */\n"
                       "static const unsigned char UPDATE_PUBKEY[%lu] = {", pubLen);
    for (ULONG i = 0; i < pubLen; i++) n += sprintf(h + n, "%s0x%02X,", i % 12 ? " " : "\n    ", pub[i]);
    n += sprintf(h + n, "\n};\n");
    if (!write_file(headerPath, h, (DWORD)n, CREATE_ALWAYS)) { fprintf(stderr, "cannot write %s\n", headerPath); return 1; }
    printf("private key: %s  (keep it secret, back it up)\npublic key:  %s\n", privPath, headerPath);
    return 0;
}

static int sign(const char *privPath, const char *version, const char *exePath, const char *sigPath)
{
    DWORD len = 0;
    unsigned char *exe = read_file(exePath, &len), digest[32], sig[64];
    if (!exe) { fprintf(stderr, "cannot read %s\n", exePath); return 1; }
    int ok = digest_of(version, exe, len, digest);
    free(exe);
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = ok ? load_private(privPath, &alg) : NULL;
    ULONG sigLen = 0;
    ok = key && NT_OK(BCryptSignHash(key, NULL, digest, 32, sig, sizeof sig, &sigLen, 0)) && sigLen == 64;
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) { fprintf(stderr, "signing failed\n"); return 1; }
    char text[256];
    int n = sprintf(text, "pauserick-update-signature v1\nversion %s\nsignature ", version);
    for (int i = 0; i < 64; i++) n += sprintf(text + n, "%02x", sig[i]);
    n += sprintf(text + n, "\n");
    if (!write_file(sigPath, text, (DWORD)n, CREATE_ALWAYS)) { fprintf(stderr, "cannot write %s\n", sigPath); return 1; }
    printf("signed %s as version %s -> %s\n", exePath, version, sigPath);
    return 0;
}

static int verify(const char *privPath, const char *version, const char *exePath, const char *sigPath)
{
    DWORD len = 0, slen = 0;
    unsigned char *exe = read_file(exePath, &len), *text = read_file(sigPath, &slen), digest[32], sig[64];
    const char *v = text ? strstr((char *)text, "\nversion ") : NULL, *s = text ? strstr((char *)text, "\nsignature ") : NULL;
    int ok = exe && v && s && !strncmp(v + 9, version, strlen(version)) && (v[9 + strlen(version)] == '\n' ||
             v[9 + strlen(version)] == '\r');
    for (int i = 0; ok && i < 64; i++) ok = sscanf(s + 11 + 2 * i, "%2hhx", &sig[i]) == 1;
    ok = ok && digest_of(version, exe, len, digest);
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = ok ? load_private(privPath, &alg) : NULL;
    ok = key && NT_OK(BCryptVerifySignature(key, NULL, digest, 32, sig, 64, 0));
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    free(exe);
    free(text);
    printf("%s\n", ok ? "signature OK" : "signature INVALID");
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "keygen")) return keygen(argv[2], argv[3]);
    if (argc == 6 && !strcmp(argv[1], "sign"))   return sign(argv[2], argv[3], argv[4], argv[5]);
    if (argc == 6 && !strcmp(argv[1], "verify")) return verify(argv[2], argv[3], argv[4], argv[5]);
    fprintf(stderr, "usage:\n  pksign keygen <private.key> <update_key.h>\n"
                    "  pksign sign   <private.key> <version> <Pauserick.exe> <Pauserick.exe.sig>\n"
                    "  pksign verify <private.key> <version> <Pauserick.exe> <Pauserick.exe.sig>\n");
    return 2;
}
