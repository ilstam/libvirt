/*
 * secret_driver.c: local driver for secret manipulation API
 *
 * Copyright (C) 2009-2016 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Red Hat Author: Miloslav Trmač <mitr@redhat.com>
 */

#include <config.h>

#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "internal.h"
#include "base64.h"
#include "datatypes.h"
#include "driver.h"
#include "virlog.h"
#include "viralloc.h"
#include "secret_conf.h"
#include "secret_driver.h"
#include "virthread.h"
#include "viruuid.h"
#include "virerror.h"
#include "virfile.h"
#include "configmake.h"
#include "virstring.h"
#include "viraccessapicheck.h"

#define VIR_FROM_THIS VIR_FROM_SECRET

VIR_LOG_INIT("secret.secret_driver");

enum { SECRET_MAX_XML_FILE = 10*1024*1024 };

/* Internal driver state */

typedef struct _virSecretObj virSecretObj;
typedef virSecretObj *virSecretObjPtr;
struct _virSecretObj {
    virSecretObjPtr next;
    virSecretDefPtr def;
    unsigned char *value;       /* May be NULL */
    size_t value_size;
};

typedef struct _virSecretDriverState virSecretDriverState;
typedef virSecretDriverState *virSecretDriverStatePtr;
struct _virSecretDriverState {
    virMutex lock;
    virSecretObj *secrets;
    char *configDir;
};

static virSecretDriverStatePtr driver;

static void
secretDriverLock(void)
{
    virMutexLock(&driver->lock);
}

static void
secretDriverUnlock(void)
{
    virMutexUnlock(&driver->lock);
}

static virSecretObjPtr
listUnlink(virSecretObjPtr *pptr)
{
    virSecretObjPtr secret;

    secret = *pptr;
    *pptr = secret->next;
    return secret;
}

static void
listInsert(virSecretObjPtr *pptr,
           virSecretObjPtr secret)
{
    secret->next = *pptr;
    *pptr = secret;
}

static void
secretFree(virSecretObjPtr secret)
{
    if (secret == NULL)
        return;

    virSecretDefFree(secret->def);
    if (secret->value != NULL) {
        memset(secret->value, 0, secret->value_size);
        VIR_FREE(secret->value);
    }
    VIR_FREE(secret);
}

static virSecretObjPtr
secretFindByUUID(const unsigned char *uuid)
{
    virSecretObjPtr *pptr, secret;

    for (pptr = &driver->secrets; *pptr != NULL; pptr = &secret->next) {
        secret = *pptr;
        if (memcmp(secret->def->uuid, uuid, VIR_UUID_BUFLEN) == 0)
            return secret;
    }
    return NULL;
}

static virSecretObjPtr
secretFindByUsage(int usageType,
                  const char *usageID)
{
    virSecretObjPtr *pptr, secret;

    for (pptr = &driver->secrets; *pptr != NULL; pptr = &secret->next) {
        secret = *pptr;

        if (secret->def->usage_type != usageType)
            continue;

        switch (usageType) {
        case VIR_SECRET_USAGE_TYPE_NONE:
            /* never match this */
            break;

        case VIR_SECRET_USAGE_TYPE_VOLUME:
            if (STREQ(secret->def->usage.volume, usageID))
                return secret;
            break;

        case VIR_SECRET_USAGE_TYPE_CEPH:
            if (STREQ(secret->def->usage.ceph, usageID))
                return secret;
            break;

        case VIR_SECRET_USAGE_TYPE_ISCSI:
            if (STREQ(secret->def->usage.target, usageID))
                return secret;
            break;
        }
    }
    return NULL;
}

/* Permament secret storage */

/* Secrets are stored in virSecretDriverStatePtr->configDir.  Each secret
   has virSecretDef stored as XML in "$basename.xml".  If a value of the
   secret is defined, it is stored as base64 (with no formatting) in
   "$basename.base64".  "$basename" is in both cases the base64-encoded UUID. */

static int
secretRewriteFile(int fd,
                  void *opaque)
{
    char *data = opaque;

    if (safewrite(fd, data, strlen(data)) < 0)
        return -1;

    return 0;
}

static char *
secretComputePath(const virSecretObj *secret,
                  const char *suffix)
{
    char *ret;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    virUUIDFormat(secret->def->uuid, uuidstr);

    ignore_value(virAsprintf(&ret, "%s/%s%s", driver->configDir,
                             uuidstr, suffix));
    return ret;
}

static char *
secretXMLPath(const virSecretObj *secret)
{
    return secretComputePath(secret, ".xml");
}

static char *
secretBase64Path(const virSecretObj *secret)
{
    return secretComputePath(secret, ".base64");
}

static int
secretEnsureDirectory(void)
{
    if (mkdir(driver->configDir, S_IRWXU) < 0 && errno != EEXIST) {
        virReportSystemError(errno, _("cannot create '%s'"),
                             driver->configDir);
        return -1;
    }
    return 0;
}

static int
secretSaveDef(const virSecretObj *secret)
{
    char *filename = NULL, *xml = NULL;
    int ret = -1;

    if (secretEnsureDirectory() < 0)
        goto cleanup;

    if (!(filename = secretXMLPath(secret)))
        goto cleanup;

    if (!(xml = virSecretDefFormat(secret->def)))
        goto cleanup;

    if (virFileRewrite(filename, S_IRUSR | S_IWUSR,
                       secretRewriteFile, xml) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_FREE(xml);
    VIR_FREE(filename);
    return ret;
}

static int
secretSaveValue(const virSecretObj *secret)
{
    char *filename = NULL, *base64 = NULL;
    int ret = -1;

    if (secret->value == NULL)
        return 0;

    if (secretEnsureDirectory() < 0)
        goto cleanup;

    if (!(filename = secretBase64Path(secret)))
        goto cleanup;

    base64_encode_alloc((const char *)secret->value, secret->value_size,
                        &base64);
    if (base64 == NULL) {
        virReportOOMError();
        goto cleanup;
    }

    if (virFileRewrite(filename, S_IRUSR | S_IWUSR,
                       secretRewriteFile, base64) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_FREE(base64);
    VIR_FREE(filename);
    return ret;
}

static int
secretDeleteSaved(const virSecretObj *secret)
{
    char *xml_filename = NULL, *value_filename = NULL;
    int ret = -1;

    if (!(xml_filename = secretXMLPath(secret)))
        goto cleanup;

    if (!(value_filename = secretBase64Path(secret)))
        goto cleanup;

    if (unlink(xml_filename) < 0 && errno != ENOENT)
        goto cleanup;
    /* When the XML is missing, the rest may waste disk space, but the secret
       won't be loaded again, so we have succeeded already. */
    ret = 0;

    (void)unlink(value_filename);

 cleanup:
    VIR_FREE(value_filename);
    VIR_FREE(xml_filename);
    return ret;
}

static int
secretLoadValidateUUID(virSecretDefPtr def,
                       const char *file)
{
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    virUUIDFormat(def->uuid, uuidstr);

    if (!virFileMatchesNameSuffix(file, uuidstr, ".xml")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("<uuid> does not match secret file name '%s'"),
                       file);
        return -1;
    }

    return 0;
}

static int
secretLoadValue(virSecretObjPtr secret)
{
    int ret = -1, fd = -1;
    struct stat st;
    char *filename = NULL, *contents = NULL, *value = NULL;
    size_t value_size;

    if (!(filename = secretBase64Path(secret)))
        goto cleanup;

    if ((fd = open(filename, O_RDONLY)) == -1) {
        if (errno == ENOENT) {
            ret = 0;
            goto cleanup;
        }
        virReportSystemError(errno, _("cannot open '%s'"), filename);
        goto cleanup;
    }

    if (fstat(fd, &st) < 0) {
        virReportSystemError(errno, _("cannot stat '%s'"), filename);
        goto cleanup;
    }

    if ((size_t)st.st_size != st.st_size) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("'%s' file does not fit in memory"), filename);
        goto cleanup;
    }

    if (VIR_ALLOC_N(contents, st.st_size) < 0)
        goto cleanup;

    if (saferead(fd, contents, st.st_size) != st.st_size) {
        virReportSystemError(errno, _("cannot read '%s'"), filename);
        goto cleanup;
    }

    VIR_FORCE_CLOSE(fd);

    if (!base64_decode_alloc(contents, st.st_size, &value, &value_size)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("invalid base64 in '%s'"), filename);
        goto cleanup;
    }
    if (value == NULL)
        goto cleanup;

    secret->value = (unsigned char *)value;
    value = NULL;
    secret->value_size = value_size;

    ret = 0;

 cleanup:
    if (value != NULL) {
        memset(value, 0, value_size);
        VIR_FREE(value);
    }
    if (contents != NULL) {
        memset(contents, 0, st.st_size);
        VIR_FREE(contents);
    }
    VIR_FORCE_CLOSE(fd);
    VIR_FREE(filename);
    return ret;
}

static virSecretObjPtr
secretLoad(const char *file,
           const char *path)
{
    virSecretDefPtr def = NULL;
    virSecretObjPtr secret = NULL, ret = NULL;

    if (!(def = virSecretDefParseFile(path)))
        goto cleanup;

    if (secretLoadValidateUUID(def, file) < 0)
        goto cleanup;

    if (VIR_ALLOC(secret) < 0)
        goto cleanup;
    secret->def = def;
    def = NULL;

    if (secretLoadValue(secret) < 0)
        goto cleanup;

    ret = secret;
    secret = NULL;

 cleanup:
    secretFree(secret);
    virSecretDefFree(def);
    return ret;
}

static int
loadSecrets(virSecretObjPtr *dest)
{
    DIR *dir = NULL;
    struct dirent *de;
    virSecretObjPtr list = NULL;

    if (!(dir = opendir(driver->configDir))) {
        if (errno == ENOENT)
            return 0;
        virReportSystemError(errno, _("cannot open '%s'"),
                             driver->configDir);
        return -1;
    }

    while (virDirRead(dir, &de, NULL) > 0) {
        char *path;
        virSecretObjPtr secret;

        if (STREQ(de->d_name, ".") || STREQ(de->d_name, ".."))
            continue;

        if (!virFileHasSuffix(de->d_name, ".xml"))
            continue;

        if (!(path = virFileBuildPath(driver->configDir, de->d_name, NULL)))
            continue;

        if (!(secret = secretLoad(de->d_name, path))) {
            virErrorPtr err = virGetLastError();

            VIR_ERROR(_("Error reading secret: %s"),
                      err != NULL ? err->message: _("unknown error"));
            virResetError(err);
            VIR_FREE(path);
            continue;
        }

        VIR_FREE(path);
        listInsert(&list, secret);
    }
    /* Ignore error reported by readdir, if any.  It's better to keep the
       secrets we managed to find. */

    while (list != NULL) {
        virSecretObjPtr secret;

        secret = listUnlink(&list);
        listInsert(dest, secret);
    }

    closedir(dir);
    return 0;
}

/* Driver functions */

static int
secretConnectNumOfSecrets(virConnectPtr conn)
{
    size_t i;
    virSecretObjPtr secret;

    if (virConnectNumOfSecretsEnsureACL(conn) < 0)
        return -1;

    secretDriverLock();

    i = 0;
    for (secret = driver->secrets; secret != NULL; secret = secret->next) {
        if (virConnectNumOfSecretsCheckACL(conn,
                                           secret->def))
            i++;
    }

    secretDriverUnlock();
    return i;
}

static int
secretConnectListSecrets(virConnectPtr conn,
                         char **uuids,
                         int maxuuids)
{
    size_t i;
    virSecretObjPtr secret;

    memset(uuids, 0, maxuuids * sizeof(*uuids));

    if (virConnectListSecretsEnsureACL(conn) < 0)
        return -1;

    secretDriverLock();

    i = 0;
    for (secret = driver->secrets; secret != NULL; secret = secret->next) {
        char *uuidstr;
        if (!virConnectListSecretsCheckACL(conn,
                                           secret->def))
            continue;
        if (i == maxuuids)
            break;
        if (VIR_ALLOC_N(uuidstr, VIR_UUID_STRING_BUFLEN) < 0)
            goto cleanup;
        virUUIDFormat(secret->def->uuid, uuidstr);
        uuids[i] = uuidstr;
        i++;
    }

    secretDriverUnlock();
    return i;

 cleanup:
    secretDriverUnlock();

    for (i = 0; i < maxuuids; i++)
        VIR_FREE(uuids[i]);

    return -1;
}

static const char *
secretUsageIDForDef(virSecretDefPtr def)
{
    switch (def->usage_type) {
    case VIR_SECRET_USAGE_TYPE_NONE:
        return "";

    case VIR_SECRET_USAGE_TYPE_VOLUME:
        return def->usage.volume;

    case VIR_SECRET_USAGE_TYPE_CEPH:
        return def->usage.ceph;

    case VIR_SECRET_USAGE_TYPE_ISCSI:
        return def->usage.target;

    default:
        return NULL;
    }
}

#define MATCH(FLAG) (flags & (FLAG))
static int
secretConnectListAllSecrets(virConnectPtr conn,
                            virSecretPtr **secrets,
                            unsigned int flags)
{
    virSecretPtr *tmp_secrets = NULL;
    int nsecrets = 0;
    int ret_nsecrets = 0;
    virSecretObjPtr secret = NULL;
    size_t i = 0;
    int ret = -1;

    virCheckFlags(VIR_CONNECT_LIST_SECRETS_FILTERS_ALL, -1);

    if (virConnectListAllSecretsEnsureACL(conn) < 0)
        return -1;

    secretDriverLock();

    for (secret = driver->secrets; secret != NULL; secret = secret->next)
        nsecrets++;

    if (secrets && VIR_ALLOC_N(tmp_secrets, nsecrets + 1) < 0)
        goto cleanup;

    for (secret = driver->secrets; secret != NULL; secret = secret->next) {
        if (!virConnectListAllSecretsCheckACL(conn,
                                              secret->def))
            continue;

        /* filter by whether it's ephemeral */
        if (MATCH(VIR_CONNECT_LIST_SECRETS_FILTERS_EPHEMERAL) &&
            !((MATCH(VIR_CONNECT_LIST_SECRETS_EPHEMERAL) &&
               secret->def->ephemeral) ||
              (MATCH(VIR_CONNECT_LIST_SECRETS_NO_EPHEMERAL) &&
               !secret->def->ephemeral)))
            continue;

        /* filter by whether it's private */
        if (MATCH(VIR_CONNECT_LIST_SECRETS_FILTERS_PRIVATE) &&
            !((MATCH(VIR_CONNECT_LIST_SECRETS_PRIVATE) &&
               secret->def->private) ||
              (MATCH(VIR_CONNECT_LIST_SECRETS_NO_PRIVATE) &&
               !secret->def->private)))
            continue;

        if (secrets) {
            if (!(tmp_secrets[ret_nsecrets] =
                  virGetSecret(conn,
                               secret->def->uuid,
                               secret->def->usage_type,
                               secretUsageIDForDef(secret->def))))
                goto cleanup;
        }
        ret_nsecrets++;
    }

    if (tmp_secrets) {
        /* trim the array to the final size */
        ignore_value(VIR_REALLOC_N(tmp_secrets, ret_nsecrets + 1));
        *secrets = tmp_secrets;
        tmp_secrets = NULL;
    }

    ret = ret_nsecrets;

 cleanup:
    secretDriverUnlock();
    if (tmp_secrets) {
        for (i = 0; i < ret_nsecrets; i ++)
            virObjectUnref(tmp_secrets[i]);
    }
    VIR_FREE(tmp_secrets);

    return ret;
}
#undef MATCH


static virSecretPtr
secretLookupByUUID(virConnectPtr conn,
                   const unsigned char *uuid)
{
    virSecretPtr ret = NULL;
    virSecretObjPtr secret;

    secretDriverLock();

    if (!(secret = secretFindByUUID(uuid))) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(uuid, uuidstr);
        virReportError(VIR_ERR_NO_SECRET,
                       _("no secret with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (virSecretLookupByUUIDEnsureACL(conn, secret->def) < 0)
        goto cleanup;

    ret = virGetSecret(conn,
                       secret->def->uuid,
                       secret->def->usage_type,
                       secretUsageIDForDef(secret->def));

 cleanup:
    secretDriverUnlock();
    return ret;
}


static virSecretPtr
secretLookupByUsage(virConnectPtr conn,
                    int usageType,
                    const char *usageID)
{
    virSecretPtr ret = NULL;
    virSecretObjPtr secret;

    secretDriverLock();

    if (!(secret = secretFindByUsage(usageType, usageID))) {
        virReportError(VIR_ERR_NO_SECRET,
                       _("no secret with matching usage '%s'"), usageID);
        goto cleanup;
    }

    if (virSecretLookupByUsageEnsureACL(conn, secret->def) < 0)
        goto cleanup;

    ret = virGetSecret(conn,
                       secret->def->uuid,
                       secret->def->usage_type,
                       secretUsageIDForDef(secret->def));

 cleanup:
    secretDriverUnlock();
    return ret;
}


static virSecretPtr
secretDefineXML(virConnectPtr conn,
                const char *xml,
                unsigned int flags)
{
    virSecretPtr ret = NULL;
    virSecretObjPtr secret;
    virSecretDefPtr backup = NULL;
    virSecretDefPtr new_attrs;

    virCheckFlags(0, NULL);

    if (!(new_attrs = virSecretDefParseString(xml)))
        return NULL;

    secretDriverLock();

    if (virSecretDefineXMLEnsureACL(conn, new_attrs) < 0)
        goto cleanup;

    if (!(secret = secretFindByUUID(new_attrs->uuid))) {
        /* No existing secret with same UUID,
         * try look for matching usage instead */
        const char *usageID = secretUsageIDForDef(new_attrs);
        if ((secret = secretFindByUsage(new_attrs->usage_type, usageID))) {
            char uuidstr[VIR_UUID_STRING_BUFLEN];
            virUUIDFormat(secret->def->uuid, uuidstr);
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("a secret with UUID %s already defined for "
                             "use with %s"),
                           uuidstr, usageID);
            goto cleanup;
        }

        /* No existing secret at all, create one */
        if (VIR_ALLOC(secret) < 0)
            goto cleanup;

        listInsert(&driver->secrets, secret);
        secret->def = new_attrs;
    } else {
        const char *newUsageID = secretUsageIDForDef(new_attrs);
        const char *oldUsageID = secretUsageIDForDef(secret->def);
        if (STRNEQ(oldUsageID, newUsageID)) {
            char uuidstr[VIR_UUID_STRING_BUFLEN];
            virUUIDFormat(secret->def->uuid, uuidstr);
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("a secret with UUID %s is already defined "
                             "for use with %s"),
                           uuidstr, oldUsageID);
            goto cleanup;
        }

        if (secret->def->private && !new_attrs->private) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("cannot change private flag on existing secret"));
            goto cleanup;
        }

        /* Got an existing secret matches attrs, so reuse that */
        backup = secret->def;
        secret->def = new_attrs;
    }

    if (!new_attrs->ephemeral) {
        if (backup && backup->ephemeral) {
            if (secretSaveValue(secret) < 0)
                goto restore_backup;
        }
        if (secretSaveDef(secret) < 0) {
            if (backup && backup->ephemeral) {
                char *filename;

                /* Undo the secretSaveValue() above; ignore errors */
                filename = secretBase64Path(secret);
                if (filename != NULL)
                    (void)unlink(filename);
                VIR_FREE(filename);
            }
            goto restore_backup;
        }
    } else if (backup && !backup->ephemeral) {
        if (secretDeleteSaved(secret) < 0)
            goto restore_backup;
    }
    /* Saved successfully - drop old values */
    new_attrs = NULL;
    virSecretDefFree(backup);

    ret = virGetSecret(conn,
                       secret->def->uuid,
                       secret->def->usage_type,
                       secretUsageIDForDef(secret->def));
    goto cleanup;

 restore_backup:
    if (backup) {
        /* Error - restore previous state and free new attributes */
        secret->def = backup;
    } else {
        /* "secret" was added to the head of the list above */
        if (listUnlink(&driver->secrets) != secret)
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("list of secrets is inconsistent"));
        else
            secretFree(secret);
    }

 cleanup:
    virSecretDefFree(new_attrs);
    secretDriverUnlock();

    return ret;
}

static char *
secretGetXMLDesc(virSecretPtr obj,
                 unsigned int flags)
{
    char *ret = NULL;
    virSecretObjPtr secret;

    virCheckFlags(0, NULL);

    secretDriverLock();

    if (!(secret = secretFindByUUID(obj->uuid))) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(obj->uuid, uuidstr);
        virReportError(VIR_ERR_NO_SECRET,
                       _("no secret with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (virSecretGetXMLDescEnsureACL(obj->conn, secret->def) < 0)
        goto cleanup;

    ret = virSecretDefFormat(secret->def);

 cleanup:
    secretDriverUnlock();

    return ret;
}

static int
secretSetValue(virSecretPtr obj,
               const unsigned char *value,
               size_t value_size,
               unsigned int flags)
{
    int ret = -1;
    unsigned char *old_value, *new_value;
    size_t old_value_size;
    virSecretObjPtr secret;

    virCheckFlags(0, -1);

    if (VIR_ALLOC_N(new_value, value_size) < 0)
        return -1;

    secretDriverLock();

    if (!(secret = secretFindByUUID(obj->uuid))) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(obj->uuid, uuidstr);
        virReportError(VIR_ERR_NO_SECRET,
                       _("no secret with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (virSecretSetValueEnsureACL(obj->conn, secret->def) < 0)
        goto cleanup;

    old_value = secret->value;
    old_value_size = secret->value_size;

    memcpy(new_value, value, value_size);
    secret->value = new_value;
    secret->value_size = value_size;
    if (!secret->def->ephemeral) {
        if (secretSaveValue(secret) < 0)
            goto restore_backup;
    }
    /* Saved successfully - drop old value */
    if (old_value != NULL) {
        memset(old_value, 0, old_value_size);
        VIR_FREE(old_value);
    }
    new_value = NULL;

    ret = 0;
    goto cleanup;

 restore_backup:
    /* Error - restore previous state and free new value */
    secret->value = old_value;
    secret->value_size = old_value_size;
    memset(new_value, 0, value_size);

 cleanup:
    secretDriverUnlock();

    VIR_FREE(new_value);

    return ret;
}

static unsigned char *
secretGetValue(virSecretPtr obj,
               size_t *value_size,
               unsigned int flags,
               unsigned int internalFlags)
{
    unsigned char *ret = NULL;
    virSecretObjPtr secret;

    virCheckFlags(0, NULL);

    secretDriverLock();

    if (!(secret = secretFindByUUID(obj->uuid))) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(obj->uuid, uuidstr);
        virReportError(VIR_ERR_NO_SECRET,
                       _("no secret with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (virSecretGetValueEnsureACL(obj->conn, secret->def) < 0)
        goto cleanup;

    if (secret->value == NULL) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(obj->uuid, uuidstr);
        virReportError(VIR_ERR_NO_SECRET,
                       _("secret '%s' does not have a value"), uuidstr);
        goto cleanup;
    }

    if ((internalFlags & VIR_SECRET_GET_VALUE_INTERNAL_CALL) == 0 &&
        secret->def->private) {
        virReportError(VIR_ERR_INVALID_SECRET, "%s",
                       _("secret is private"));
        goto cleanup;
    }

    if (VIR_ALLOC_N(ret, secret->value_size) < 0)
        goto cleanup;
    memcpy(ret, secret->value, secret->value_size);
    *value_size = secret->value_size;

 cleanup:
    secretDriverUnlock();

    return ret;
}

static int
secretUndefine(virSecretPtr obj)
{
    int ret = -1;
    virSecretObjPtr secret;

    secretDriverLock();

    if (!(secret = secretFindByUUID(obj->uuid))) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(obj->uuid, uuidstr);
        virReportError(VIR_ERR_NO_SECRET,
                       _("no secret with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (virSecretUndefineEnsureACL(obj->conn, secret->def) < 0)
        goto cleanup;

    if (!secret->def->ephemeral &&
        secretDeleteSaved(secret) < 0)
        goto cleanup;

    if (driver->secrets == secret) {
        driver->secrets = secret->next;
    } else {
        virSecretObjPtr tmp = driver->secrets;
        while (tmp && tmp->next != secret)
            tmp = tmp->next;
        if (tmp)
            tmp->next = secret->next;
    }
    secretFree(secret);

    ret = 0;

 cleanup:
    secretDriverUnlock();

    return ret;
}

static int
secretStateCleanup(void)
{
    if (driver == NULL)
        return -1;

    secretDriverLock();

    while (driver->secrets != NULL) {
        virSecretObjPtr secret;

        secret = listUnlink(&driver->secrets);
        secretFree(secret);
    }
    VIR_FREE(driver->configDir);

    secretDriverUnlock();
    virMutexDestroy(&driver->lock);
    VIR_FREE(driver);

    return 0;
}

static int
secretStateInitialize(bool privileged,
                      virStateInhibitCallback callback ATTRIBUTE_UNUSED,
                      void *opaque ATTRIBUTE_UNUSED)
{
    char *base = NULL;

    if (VIR_ALLOC(driver) < 0)
        return -1;

    if (virMutexInit(&driver->lock) < 0) {
        VIR_FREE(driver);
        return -1;
    }
    secretDriverLock();

    if (privileged) {
        if (VIR_STRDUP(base, SYSCONFDIR "/libvirt") < 0)
            goto error;
    } else {
        if (!(base = virGetUserConfigDirectory()))
            goto error;
    }
    if (virAsprintf(&driver->configDir, "%s/secrets", base) < 0)
        goto error;
    VIR_FREE(base);

    if (loadSecrets(&driver->secrets) < 0)
        goto error;

    secretDriverUnlock();
    return 0;

 error:
    VIR_FREE(base);
    secretDriverUnlock();
    secretStateCleanup();
    return -1;
}

static int
secretStateReload(void)
{
    virSecretObjPtr new_secrets = NULL;

    if (!driver)
        return -1;

    secretDriverLock();

    if (loadSecrets(&new_secrets) < 0)
        goto end;

    /* Keep ephemeral secrets from current state.
     * Discard non-ephemeral secrets that were removed
     * by the secrets configDir.  */
    while (driver->secrets != NULL) {
        virSecretObjPtr secret;

        secret = listUnlink(&driver->secrets);
        if (secret->def->ephemeral)
            listInsert(&new_secrets, secret);
        else
            secretFree(secret);
    }
    driver->secrets = new_secrets;

 end:
    secretDriverUnlock();
    return 0;
}

static virSecretDriver secretDriver = {
    .name = "secret",
    .connectNumOfSecrets = secretConnectNumOfSecrets, /* 0.7.1 */
    .connectListSecrets = secretConnectListSecrets, /* 0.7.1 */
    .connectListAllSecrets = secretConnectListAllSecrets, /* 0.10.2 */
    .secretLookupByUUID = secretLookupByUUID, /* 0.7.1 */
    .secretLookupByUsage = secretLookupByUsage, /* 0.7.1 */
    .secretDefineXML = secretDefineXML, /* 0.7.1 */
    .secretGetXMLDesc = secretGetXMLDesc, /* 0.7.1 */
    .secretSetValue = secretSetValue, /* 0.7.1 */
    .secretGetValue = secretGetValue, /* 0.7.1 */
    .secretUndefine = secretUndefine, /* 0.7.1 */
};

static virStateDriver stateDriver = {
    .name = "secret",
    .stateInitialize = secretStateInitialize,
    .stateCleanup = secretStateCleanup,
    .stateReload = secretStateReload,
};

int
secretRegister(void)
{
    if (virSetSharedSecretDriver(&secretDriver) < 0)
        return -1;
    if (virRegisterStateDriver(&stateDriver) < 0)
        return -1;
    return 0;
}
