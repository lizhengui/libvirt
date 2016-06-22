/*
 * virstorageencryption.c: volume encryption information
 *
 * Copyright (C) 2009-2014 Red Hat, Inc.
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

#include <fcntl.h>
#include <unistd.h>

#include "internal.h"

#include "virbuffer.h"
#include "viralloc.h"
#include "virstorageencryption.h"
#include "virxml.h"
#include "virerror.h"
#include "viruuid.h"
#include "virfile.h"

#define VIR_FROM_THIS VIR_FROM_STORAGE

VIR_ENUM_IMPL(virStorageEncryptionSecret,
              VIR_STORAGE_ENCRYPTION_SECRET_TYPE_LAST, "passphrase")

VIR_ENUM_IMPL(virStorageEncryptionFormat,
              VIR_STORAGE_ENCRYPTION_FORMAT_LAST,
              "default", "qcow")

static void
virStorageEncryptionSecretFree(virStorageEncryptionSecretPtr secret)
{
    if (!secret)
        return;
    VIR_FREE(secret);
}

void
virStorageEncryptionFree(virStorageEncryptionPtr enc)
{
    size_t i;

    if (!enc)
        return;

    for (i = 0; i < enc->nsecrets; i++)
        virStorageEncryptionSecretFree(enc->secrets[i]);
    VIR_FREE(enc->secrets);
    VIR_FREE(enc);
}

static virStorageEncryptionSecretPtr
virStorageEncryptionSecretCopy(const virStorageEncryptionSecret *src)
{
    virStorageEncryptionSecretPtr ret;

    if (VIR_ALLOC(ret) < 0)
        return NULL;

    memcpy(ret, src, sizeof(*src));

    return ret;
}

virStorageEncryptionPtr
virStorageEncryptionCopy(const virStorageEncryption *src)
{
    virStorageEncryptionPtr ret;
    size_t i;

    if (VIR_ALLOC(ret) < 0)
        return NULL;

    if (VIR_ALLOC_N(ret->secrets, src->nsecrets) < 0)
        goto error;

    ret->nsecrets = src->nsecrets;
    ret->format = src->format;

    for (i = 0; i < src->nsecrets; i++) {
        if (!(ret->secrets[i] = virStorageEncryptionSecretCopy(src->secrets[i])))
            goto error;
    }

    return ret;

 error:
    virStorageEncryptionFree(ret);
    return NULL;
}

static virStorageEncryptionSecretPtr
virStorageEncryptionSecretParse(xmlXPathContextPtr ctxt,
                                xmlNodePtr node)
{
    xmlNodePtr old_node;
    virStorageEncryptionSecretPtr ret;
    char *type_str = NULL;
    char *uuidstr = NULL;

    if (VIR_ALLOC(ret) < 0)
        return NULL;

    old_node = ctxt->node;
    ctxt->node = node;

    if (!(type_str = virXPathString("string(./@type)", ctxt))) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("unknown volume encryption secret type"));
        goto cleanup;
    }

    if ((ret->type = virStorageEncryptionSecretTypeFromString(type_str)) < 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("unknown volume encryption secret type %s"),
                       type_str);
        goto cleanup;
    }
    VIR_FREE(type_str);

    if ((uuidstr = virXPathString("string(./@uuid)", ctxt))) {
        if (virUUIDParse(uuidstr, ret->uuid) < 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("malformed volume encryption uuid '%s'"),
                           uuidstr);
            goto cleanup;
        }
        VIR_FREE(uuidstr);
    } else {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("missing volume encryption uuid"));
        goto cleanup;
    }
    ctxt->node = old_node;
    return ret;

 cleanup:
    VIR_FREE(type_str);
    virStorageEncryptionSecretFree(ret);
    VIR_FREE(uuidstr);
    ctxt->node = old_node;
    return NULL;
}

static virStorageEncryptionPtr
virStorageEncryptionParseXML(xmlXPathContextPtr ctxt)
{
    xmlNodePtr *nodes = NULL;
    virStorageEncryptionPtr ret;
    char *format_str = NULL;
    int n;
    size_t i;

    if (VIR_ALLOC(ret) < 0)
        return NULL;

    if (!(format_str = virXPathString("string(./@format)", ctxt))) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("unknown volume encryption format"));
        goto cleanup;
    }

    if ((ret->format =
         virStorageEncryptionFormatTypeFromString(format_str)) < 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("unknown volume encryption format type %s"),
                       format_str);
        goto cleanup;
    }
    VIR_FREE(format_str);

    if ((n = virXPathNodeSet("./secret", ctxt, &nodes)) < 0)
        goto cleanup;

    if (n > 0) {
        if (VIR_ALLOC_N(ret->secrets, n) < 0)
            goto cleanup;
        ret->nsecrets = n;

        for (i = 0; i < n; i++) {
            if (!(ret->secrets[i] =
                  virStorageEncryptionSecretParse(ctxt, nodes[i])))
                goto cleanup;
        }
        VIR_FREE(nodes);
    }

    return ret;

 cleanup:
    VIR_FREE(format_str);
    VIR_FREE(nodes);
    virStorageEncryptionFree(ret);
    return NULL;
}

virStorageEncryptionPtr
virStorageEncryptionParseNode(xmlDocPtr xml, xmlNodePtr root)
{
    xmlXPathContextPtr ctxt = NULL;
    virStorageEncryptionPtr enc = NULL;

    if (STRNEQ((const char *) root->name, "encryption")) {
        virReportError(VIR_ERR_XML_ERROR,
                       "%s", _("unknown root element for volume "
                               "encryption information"));
        goto cleanup;
    }

    ctxt = xmlXPathNewContext(xml);
    if (ctxt == NULL) {
        virReportOOMError();
        goto cleanup;
    }

    ctxt->node = root;
    enc = virStorageEncryptionParseXML(ctxt);

 cleanup:
    xmlXPathFreeContext(ctxt);
    return enc;
}


static int
virStorageEncryptionSecretFormat(virBufferPtr buf,
                                 virStorageEncryptionSecretPtr secret)
{
    const char *type;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    if (!(type = virStorageEncryptionSecretTypeToString(secret->type))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("unexpected volume encryption secret type"));
        return -1;
    }

    virUUIDFormat(secret->uuid, uuidstr);
    virBufferAsprintf(buf, "<secret type='%s' uuid='%s'/>\n",
                      type, uuidstr);
    return 0;
}

int
virStorageEncryptionFormat(virBufferPtr buf,
                           virStorageEncryptionPtr enc)
{
    const char *format;
    size_t i;

    if (!(format = virStorageEncryptionFormatTypeToString(enc->format))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("unexpected encryption format"));
        return -1;
    }
    virBufferAsprintf(buf, "<encryption format='%s'>\n", format);
    virBufferAdjustIndent(buf, 2);

    for (i = 0; i < enc->nsecrets; i++) {
        if (virStorageEncryptionSecretFormat(buf, enc->secrets[i]) < 0)
            return -1;
    }

    virBufferAdjustIndent(buf, -2);
    virBufferAddLit(buf, "</encryption>\n");

    return 0;
}

int
virStorageGenerateQcowPassphrase(unsigned char *dest)
{
    int fd;
    size_t i;

    /* A qcow passphrase is up to 16 bytes, with any data following a NUL
       ignored.  Prohibit control and non-ASCII characters to avoid possible
       unpleasant surprises with the qemu monitor input mechanism. */
    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot open /dev/urandom"));
        return -1;
    }
    i = 0;
    while (i < VIR_STORAGE_QCOW_PASSPHRASE_SIZE) {
        ssize_t r;

        while ((r = read(fd, dest + i, 1)) == -1 && errno == EINTR)
            ;
        if (r <= 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Cannot read from /dev/urandom"));
            VIR_FORCE_CLOSE(fd);
            return -1;
        }
        if (dest[i] >= 0x20 && dest[i] <= 0x7E)
            i++; /* Got an acceptable character */
    }
    VIR_FORCE_CLOSE(fd);
    return 0;
}