/************************************************************************
**
**  Copyright (C) 2015-2025 Kevin B. Hendricks  Stratford, ON Canada
**  Copyright (C) 2013      John Schember <john@nachtimwald.com>
**  Copyright (C) 2009-2011 Strahinja Markovic  <strahinja.markovic@gmail.com>
**
**  This file is part of Sigil.
**
**  Sigil is free software: you can redistribute it and/or modify
**  it under the terms of the GNU General Public License as published by
**  the Free Software Foundation, either version 3 of the License, or
**  (at your option) any later version.
**
**  Sigil is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**  GNU General Public License for more details.
**
**  You should have received a copy of the GNU General Public License
**  along with Sigil.  If not, see <http://www.gnu.org/licenses/>.
**
*************************************************************************/

#include <memory>

#include <QtCore/QBuffer>
#include <QtCore/QDate>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QUuid>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QDateTime>
#include <QDebug>
#if QT_VERSION >= QT_VERSION_CHECK(6,5,0)
#include <QTimeZone>
#endif

#include "BookManipulation/CleanSource.h"
#include "BookManipulation/XhtmlDoc.h"
#include "BookManipulation/FolderKeeper.h"
#include "Misc/Utility.h"
#include "Misc/SettingsStore.h"
#include "Misc/GuideItems.h"
#include "Misc/Landmarks.h"
#include "Misc/MediaTypes.h"
#include "ResourceObjects/HTMLResource.h"
#include "ResourceObjects/ImageResource.h"
#include "ResourceObjects/NCXResource.h"
#include "ResourceObjects/OPFResource.h"
#include "ResourceObjects/NavProcessor.h"
#include "EmbedPython/PythonRoutines.h"
#include "sigil_constants.h"
#include "sigil_exception.h"

static const QString MEDIA_PLAYBACK_ACTIVE_CLASS = "media:playback-active-class";
static const QString MEDIA_ACTIVE_CLASS          = "media:active-class";
static const QString SIGIL_VERSION_META_NAME  = "Sigil version";
static const QString OPF_XML_NAMESPACE        = "http://www.idpf.org/2007/opf";
static const QString FALLBACK_MIMETYPE        = "text/plain";
static const QString ITEM_ELEMENT_TEMPLATE    = "<item id=\"%1\" href=\"%2\" media-type=\"%3\"/>";
static const QString ITEMREF_ELEMENT_TEMPLATE = "<itemref idref=\"%1\"/>";
static const QString OPF_REWRITTEN_COMMENT    = "<!-- Your OPF file was broken so Sigil "
                                                "tried to rebuild it for you. -->";
static const QString _RS = QString(QChar(30)); // Ascii Record Separator

static const QString PKG_VERSION = "<\\s*package[^>]*version\\s*=\\s*[\"\']([^\'\"]*)[\'\"][^>]*>";

static const QString TEMPLATE_TEXT =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"BookId\">\n\n"
    "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:opf=\"http://www.idpf.org/2007/opf\">\n"
    "    <dc:identifier opf:scheme=\"UUID\" id=\"BookId\">urn:uuid:%1</dc:identifier>\n"
    "    <dc:language>%2</dc:language>\n"
    "    <dc:title>%3</dc:title>\n"
    "  </metadata>\n\n"
    "  <manifest>\n"
    "  </manifest>\n\n"
    "  <spine>\n"
    "  </spine>\n\n"
    "</package>";

/** 
 ** Epub 3 reserved prefix values for the package tag.
 ** See http://www.idpf.org/epub/vocab/package/pfx/
 **
 ** Prefix      IRI
 ** ---------   ---------------------------------------------------------
 ** dcterms     http://purl.org/dc/terms/
 ** epubsc      http://idpf.org/epub/vocab/sc/#
 ** marc        http://id.loc.gov/vocabulary/
 ** media       http://www.idpf.org/epub/vocab/overlays/#
 ** onix        http://www.editeur.org/ONIX/book/codelists/current.html#
 ** rendition   http://www.idpf.org/vocab/rendition/#
 ** schema      http://schema.org/
 ** xsd         http://www.w3.org/2001/XMLSchema#
 **
 **
 ** Note single space is required after ":" that delimits the prefix
 **
 ** example: 
 **
 ** <package … 
 **          prefix="foaf: http://xmlns.com/foaf/spec/
 **                  dbp: http://dbpedia.org/ontology/">
 **
 */

//     "    <item id=\"nav\" href=\"Text/nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
//     "    <itemref idref=\"nav\" linear=\"no\" />\n"


static const QString TEMPLATE3_TEXT =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<package version=\"3.0\" unique-identifier=\"BookId\" xmlns=\"http://www.idpf.org/2007/opf\">\n\n"
    "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
    "    <dc:identifier id=\"BookId\">urn:uuid:%1</dc:identifier>\n"
    "    <dc:language>%2</dc:language>\n"
    "    <dc:title>%3</dc:title>\n"
    "    <meta property=\"dcterms:modified\">%4</meta>\n"
    "  </metadata>\n\n"
    "  <manifest>\n"
    "  </manifest>\n\n"
    "  <spine>\n"
    "  </spine>\n\n"
    "</package>";


OPFResource::OPFResource(const QString &mainfolder, 
             const QString &fullfilepath, 
             const QString &version, 
             QObject *parent)
  : XMLResource(mainfolder, fullfilepath, parent),
    m_NavResource(NULL),
    m_WarnedAboutVersion(false)
{
    FillWithDefaultText(version);
    // Make sure the file exists on disk.
    // Among many reasons, this also solves the problem
    // with the Book Browser not displaying an icon for this resource.
    SaveToDisk();
}


// just renaming the opf (but not moving it) should not trigger
// a need for other source updates
// but we will need to rewrite the META-INF/container.xml
bool OPFResource::RenameTo(const QString &new_filename, bool in_bulk)
{
    Q_UNUSED(in_bulk);
    bool successful = Resource::RenameTo(new_filename);
    if (successful) {
        FolderKeeper::UpdateContainerXML(GetFullPathToBookFolder(), GetRelativePath());
    }
    return successful;
}


// moving the opf should trigger a need for many source updates
// and the need to rewrite the META-INF/container.xml
bool OPFResource::MoveTo(const QString &newbookpath, bool in_bulk)
{
    Q_UNUSED(in_bulk);
    bool successful = Resource::MoveTo(newbookpath);
    if (successful) {
        FolderKeeper::UpdateContainerXML(GetFullPathToBookFolder(), GetRelativePath());
    }
    return successful;
}


Resource::ResourceType OPFResource::Type() const
{
    return Resource::OPFResourceType;
}


QString OPFResource::GetText() const
{
    return TextResource::GetText();
}


void OPFResource::SetText(const QString &text)
{
    emit TextChanging();
    QWriteLocker locker(&GetLock());
    QString source = ValidatePackageVersion(text);
    TextResource::SetText(source);
}


bool OPFResource::LoadFromDisk()
{
    try {
        const QString &text = Utility::ReadUnicodeTextFile(GetFullPath());
        SetText(text);
        emit LoadedFromDisk();
        return true;
    } catch (CannotOpenFile&) {
        //
    }
    return false;
}

QList<Resource*> OPFResource::GetSpineOrderResources( const QList<Resource *> &resources)
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    const QHash<QString, Resource*> id_mapping = GetManifestIDResourceMapping(resources, p);
    QList<Resource *> spine_order;
    for (int i = 0; i < p.m_spine.count(); ++i) {
        QString idref = p.m_spine.at(i).m_idref;
        if (id_mapping.contains(idref)) {
            spine_order << id_mapping[idref];
        }
    }
    return spine_order;
}

QHash <Resource *, int>  OPFResource::GetReadingOrderAll( const QList <Resource *> resources)
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    QHash <Resource *, int> reading_order;
    QHash<QString, int> id_order;
    for (int i = 0; i < p.m_spine.count(); ++i) {
        id_order[p.m_spine.at(i).m_idref] = i;
    }
    QHash<Resource *, QString> id_mapping = GetResourceManifestIDMapping(resources, p);
    foreach(Resource *resource, resources) {
        reading_order[resource] = id_order[id_mapping[resource]];
    }
    return reading_order;
}


int OPFResource::GetReadingOrder(const HTMLResource *html_resource) const
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    const Resource *resource = static_cast<const Resource *>(html_resource);
    QString resource_id = GetResourceManifestID(resource, p);
    for (int i = 0; i < p.m_spine.count(); ++i) {
        QString idref = p.m_spine.at(i).m_idref;
        if (resource_id == idref) {
            return i;
        }
    }
    return -1;
}

void OPFResource::MoveReadingOrder(const HTMLResource* from_resource, const HTMLResource* after_resource)
{
    QWriteLocker locker(&GetLock());
    const Resource *from_res = static_cast<const Resource *>(from_resource);
    const Resource *after_res = static_cast<const Resource *>(after_resource);
    if (from_res == NULL || after_res == NULL) return;

    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    QString from_id = GetResourceManifestID(from_res, p);
    QString after_id = GetResourceManifestID(after_res, p);

    int from_pos = -1;
    int after_pos = -1;
    int spine_count = p.m_spine.count();
    for (int i = 0; i < spine_count; i++) {
        QString aref = p.m_spine.at(i).m_idref;
        if (aref == from_id) from_pos = i;
        if (aref == after_id) after_pos = i;
    }

    if ((from_pos < 0) || (after_pos < 0)) return;
    if (from_pos >= spine_count || after_pos >= spine_count) return;
    // a move is equivalent to the sequence insert(after, takeAt(from))
    // if takeAt pos <  after pos, it effectively increases the after position by one
    // if takeAt pos > after pos, you need to increment after pos by 1
    if (from_pos != after_pos + 1) {
        if (from_pos > after_pos) after_pos += 1;
        p.m_spine.move(from_pos, after_pos);
    }
    UpdateText(p);
}


QString OPFResource::GetMainIdentifierValue() const
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    int i = GetMainIdentifier(p);
    if (i > -1) {
        return QString(p.m_metadata.at(i).m_content);
    }
    return QString();
}

void OPFResource::SaveToDisk(bool book_wide_save)
{
    QString source = ValidatePackageVersion(CleanSource::ProcessXML(GetText(),"application/oebps-package+xml"));
    // Work around for covers appearing on the Nook. Issue 942.
    source = source.replace(QRegularExpression("<meta content=\"([^\"]+)\" name=\"cover\""), "<meta name=\"cover\" content=\"\\1\"");
    TextResource::SetText(source);
    TextResource::SaveToDisk(book_wide_save);
}


QString OPFResource::GetPackageVersion() const
{
    QReadLocker locker(&GetLock());
    // The right way to do this is to properly parse the opf.
    //  That means invoking the embedded python code and lxml.
    // As this code will be called many times and from many places
    // convert it to a simple QRegualr expression query for speed.

    // QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    // OPFParser p;
    // p.parse(source);
    // return p.m_package.m_version;

    QString opftext = GetText();
    QRegularExpression pkgversion_search(PKG_VERSION, QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch pkgversion_mo = pkgversion_search.match(opftext);
    QString version = "2.0";
    if (pkgversion_mo.hasMatch()) {
        version = pkgversion_mo.captured(1);
    }
    return version;
}


QString OPFResource::GetUUIDIdentifierValue()
{
    EnsureUUIDIdentifierPresent();
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    for (int i=0; i < p.m_metadata.count(); ++i) {
        MetaEntry me = p.m_metadata.at(i);
        if(me.m_name.startsWith("dc:identifier")) {
            QString value = QString(me.m_content).remove("urn:uuid:");
            if (!QUuid(value).isNull()) {
                return value;
            }
        }
    }
    // EnsureUUIDIdentifierPresent should ensure we
    // never reach here.
    Q_ASSERT(false);
    return QString();
}


void OPFResource::EnsureUUIDIdentifierPresent()
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    for (int i=0; i < p.m_metadata.count(); ++i) {
        MetaEntry me = p.m_metadata.at(i);
        if(me.m_name.startsWith("dc:identifier")) {
            QString value = QString(me.m_content).remove("urn:uuid:");
            if (!QUuid(value).isNull()) {
                return;
            }
        }
    }
    QString uuid = Utility::CreateUUID();
    // add in the proper identifier type prefix
    if (!uuid.startsWith("urn:uuid:")) {
        uuid = "urn:uuid:" + uuid;
    }
    WriteIdentifier("UUID", uuid, p);
    UpdateText(p);
}

// This routine add the NCX to the OPF Mainifest
// ncx_path is the full absolute file path to the ncx
QString OPFResource::AddNCXItem(const QString &ncx_path, QString id)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    QString ncx_bkpath = ncx_path.right(ncx_path.length() - GetFullPathToBookFolder().length() - 1);
    QString ncx_rel_path = Utility::buildRelativePath(GetRelativePath(), ncx_bkpath);
    int n = p.m_manifest.count();
    ManifestEntry me;
    me.m_id = GetUniqueID(id, p);
    me.m_href = Utility::URLEncodePath(ncx_rel_path);
    me.m_mtype = "application/x-dtbncx+xml";
    p.m_manifest.append(me);
    p.m_idpos[me.m_id] = n;
    p.m_hrefpos[me.m_href] = n;
    UpdateText(p);
    return me.m_id;
}


void OPFResource::UpdateNCXOnSpine(const QString &new_ncx_id)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    QString ncx_id = p.m_spineattr.m_atts.value(QString("toc"),"");
    if (new_ncx_id != ncx_id) {
        p.m_spineattr.m_atts[QString("toc")] = new_ncx_id;
        UpdateText(p);
    }
}

void OPFResource::RemoveNCXOnSpine()
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    p.m_spineattr.m_atts.remove("toc");
    UpdateText(p);
}


void OPFResource::UpdateNCXLocationInManifest(const NCXResource *ncx)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    QString ncx_id = p.m_spineattr.m_atts.value(QString("toc"), "");
    int pos = p.m_idpos.value(ncx_id, -1);
    if (pos > -1) {
        ManifestEntry me = p.m_manifest.at(pos);
        QString href = me.m_href;
        QString new_href = Utility::URLEncodePath(GetRelativePathToResource(ncx));
        me.m_href = new_href;
        p.m_manifest.replace(pos, me);
        p.m_hrefpos.remove(href);
        p.m_hrefpos[new_href] = pos;
        UpdateText(p);
    }
}


void OPFResource::AddSigilVersionMeta()
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    for (int i=0; i < p.m_metadata.count(); ++i) {
        MetaEntry me = p.m_metadata.at(i);
        if ((me.m_name == "meta") && (me.m_atts.contains("name"))) {  
            QString name = me.m_atts[QString("name")];
            if (name == SIGIL_VERSION_META_NAME) {
                me.m_atts["content"] = QString(SIGIL_VERSION);
                p.m_metadata.replace(i, me);
                UpdateText(p);
                return;
            }
        }
    }
    MetaEntry me;
    me.m_name = "meta";
    me.m_atts[QString("name")] = QString("Sigil version");
    me.m_atts[QString("content")] = QString(SIGIL_VERSION);
    p.m_metadata.append(me);
    UpdateText(p);
}


bool OPFResource::IsCoverImage(const ImageResource *image_resource) const
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    QString resource_id = GetResourceManifestID(image_resource, p);
    return IsCoverImageCheck(resource_id, p);
}


bool OPFResource::IsCoverImageCheck(QString resource_id, const OPFParser & p) const
{
    int pos = GetCoverMeta(p);
    if (pos > -1) {
        MetaEntry me = p.m_metadata.at(pos);
        return me.m_atts.value(QString("content"),QString("")) == resource_id;
    }
    return false;
}


bool OPFResource::CoverImageExists() const
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    return GetCoverMeta(p) > -1;
}


void OPFResource::AutoFixWellFormedErrors()
{
    QWriteLocker locker(&GetLock());
    const QStringList TEXT_EXTS = QStringList() << "htm" << "html" << "xhtml";
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    // auto fill in spine from manifest if completely empty
    if (p.m_spine.count() == 0) {
        std::vector< std::pair< QString, QString > > txts;
        for (int i=0; i < p.m_manifest.count(); i++) {
            ManifestEntry me = p.m_manifest.at(i);
            QString ahref = Utility::URLDecodePath(me.m_href);
            QString aid = me.m_id;
            QString ext = QFileInfo(ahref).suffix();
            if ((me.m_mtype == "application/xhtml+xml") || TEXT_EXTS.contains(ext.toLower())) {
                txts.push_back(std::make_pair(ahref, aid));
            }
        }
        std::sort(txts.begin(), txts.end(), Utility::sort_string_pairs_by_first);
        for (unsigned int j=0; j < txts.size(); j++) {
            QString idref = txts.at(j).second;
            SpineEntry sp;
            sp.m_idref = idref;
            p.m_spine << sp;
        }
    }
    UpdateText(p);
}


QStringList OPFResource::GetSpineOrderBookPaths() const
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    QStringList book_paths_in_reading_order;
    for (int i=0; i < p.m_spine.count(); ++i) {
        SpineEntry sp = p.m_spine.at(i);
        QString idref = sp.m_idref;
        int pos = p.m_idpos.value(idref,-1);
        if (pos > -1) {
            QString apath = Utility::URLDecodePath(p.m_manifest.at(pos).m_href);
            book_paths_in_reading_order.append(Utility::buildBookPath(apath,GetFolder()));
        }
    }
    return book_paths_in_reading_order;
}


QStringList OPFResource::GetMediaOverlayActiveClassSelectors() const
{
    QReadLocker locker(&GetLock());
    QStringList activeclassselectors;
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    for (int i=0; i < p.m_metadata.count(); ++i) {
        if (p.m_metadata.at(i).m_name == "meta") {
            MetaEntry me = p.m_metadata.at(i);
            QString propval = me.m_atts.value("property", "");
            if (propval == MEDIA_ACTIVE_CLASS) {
                activeclassselectors << "." + me.m_content;
            }
            if (propval == MEDIA_PLAYBACK_ACTIVE_CLASS) {
                activeclassselectors << "." + me.m_content;
            }
        }
    }
    return activeclassselectors;
}


QString OPFResource::GetPrimaryBookTitle() const
{
    QString title = "";
    QStringList titles = GetDCMetadataValues("dc:title");
    if (!titles.isEmpty()) {
        title = titles.at(0);
    }
    return title;
}

QString OPFResource::GetPrimaryBookLanguage() const
{
    SettingsStore settings;
    QString lang = settings.defaultMetadataLang();
    QStringList languages = GetDCMetadataValues("dc:language");
    if (!languages.isEmpty()) {
        lang = languages.at(0);
    }
    return lang;
}

QList<MetaEntry> OPFResource::GetDCMetadata() const
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    QList<MetaEntry> metadata;
    for (int i=0; i < p.m_metadata.count(); ++i) {
        if (p.m_metadata.at(i).m_name.startsWith("dc:")) {
            MetaEntry me(p.m_metadata.at(i));
            metadata.append(me);
        }
    }
    return metadata;
}


QString OPFResource::GetMetadataXML() const
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    return p.get_metadata_xml();
}


QStringList OPFResource::GetDCMetadataValues(QString text) const
{
    QStringList metavalues;
    foreach(MetaEntry meta, GetDCMetadata()) {
        if (meta.m_name == text) {
            metavalues.append(meta.m_content);
        }
    }
    return metavalues;
}


void OPFResource::SetDCMetadata(const QList<MetaEntry> &metadata)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    // this will not work with refines so it needs to be fixed
    RemoveDCElements(p);
    foreach(MetaEntry book_meta, metadata) {
        MetaEntry me(book_meta);
        me.m_content = me.m_content.toHtmlEscaped();
        p.m_metadata.append(me);
    }
    UpdateText(p);
}


void OPFResource::AddResource(const Resource *resource)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    ManifestEntry me;
    me.m_id = GetUniqueID(GetValidID(resource->Filename()),p);
    me.m_href = Utility::URLEncodePath(GetRelativePathToResource(resource));
    me.m_mtype = GetResourceMimetype(resource);
    // Argh! If this is an new blank resource - it will have no content yet
    // so trying to parse it here to check for manifest properties is a mistake
    int n = p.m_manifest.count();
    p.m_manifest.append(me);
    p.m_idpos[me.m_id] = n;
    p.m_hrefpos[me.m_href] = n;
    if (resource->Type() == Resource::HTMLResourceType) {
        SpineEntry se;
        se.m_idref = me.m_id;
        p.m_spine.append(se);
    }
    UpdateText(p);
}

void OPFResource::RemoveCoverImageProperty(QString& resource_id, OPFParser& p)
{
    // remove the cover image property from manifest with resource_id
    if (!resource_id.isEmpty()) {
        int pos = p.m_idpos.value(resource_id, -1);
        if (pos >= 0 ) {
            ManifestEntry me = p.m_manifest.at(p.m_idpos[resource_id]);
            QString properties = me.m_atts.value("properties", "");
            if (properties.contains("cover-image")) {
                properties = properties.remove("cover-image");
                properties = properties.simplified();
            }
            me.m_atts.remove("properties");
            if (!properties.isEmpty()) {
                me.m_atts["properties"] = properties;
            }
            p.m_manifest.replace(pos, me);
        }
    }
}


void OPFResource::AddCoverImageProperty(QString& resource_id, OPFParser& p)
{
    // add the cover image property from manifest with resource_id
    if (!resource_id.isEmpty()) {
        int pos = p.m_idpos.value(resource_id, -1);
        if (pos >= 0 ) {
            ManifestEntry me = p.m_manifest.at(p.m_idpos[resource_id]);
            QString properties = me.m_atts.value("properties", "cover-image");
            if (!properties.contains("cover-image")) {
                properties = properties.append(" cover-image");
            }
            me.m_atts.remove("properties");
            me.m_atts["properties"] = properties;
            p.m_manifest.replace(pos, me);
        }
    }
}


void OPFResource::RemoveCoverMetaForImage(const Resource *resource, OPFParser& p)
{
    int pos = GetCoverMeta(p);
    QString resource_id = GetResourceManifestID(resource, p);

    // Remove entry if there is a cover in meta and if this file is marked as cover
    if (pos > -1) {
       MetaEntry me = p.m_metadata.at(pos);
       if (me.m_atts.value(QString("content"),QString("")) == resource_id) {
           p.m_metadata.removeAt(pos);
       }
    }
}

void OPFResource::AddCoverMetaForImage(const Resource *resource, OPFParser &p)
{
    int pos = GetCoverMeta(p);
    QString resource_id = GetResourceManifestID(resource, p);

    // If a cover entry exists, update its id, else create one
    if (pos > -1) {
        MetaEntry me = p.m_metadata.at(pos);
        me.m_atts["content"] = resource_id;
        p.m_metadata.replace(pos, me);
    } else {
        MetaEntry me;
        me.m_name = "meta";
        me.m_atts["name"] = "cover";
        me.m_atts["content"] = QString(resource_id);
        p.m_metadata.append(me);
    }
}

void OPFResource::BulkRemoveResources(const QList<Resource *>resources)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    if (p.m_manifest.isEmpty()) return;

    foreach(Resource * resource, resources) {
        QString href = Utility::URLEncodePath(GetRelativePathToResource(resource));
        int pos = p.m_hrefpos.value(href, -1);
        QString item_id = "";

        // Delete the meta tag for cover images before deleting the manifest entry
        if (resource->Type() == Resource::ImageResourceType) {
            RemoveCoverMetaForImage(resource, p);
        }
        if (pos > -1) {
            item_id = p.m_manifest.at(pos).m_id;
        }
        if (resource->Type() == Resource::HTMLResourceType) {
            for (int i=0; i < p.m_spine.count(); ++i) {
                QString idref = p.m_spine.at(i).m_idref;
                if (idref == item_id) {
                    p.m_spine.removeAt(i);
                    break;
                }
            }
            RemoveAllGuideReferencesForResource(resource, p);
            QString version = GetEpubVersion();
            if (version.startsWith('3')) {
                NavProcessor navproc(GetNavResource());
                navproc.RemoveAllLandmarksForResource(resource);
            }
        }
        if (pos > -1) {
            p.m_manifest.removeAt(pos);
            // rebuild the maps since updating them item by item would be slower
            p.m_idpos.clear();
            p.m_hrefpos.clear();
            for (int i=0; i < p.m_manifest.count(); ++i) {
                p.m_idpos[p.m_manifest.at(i).m_id] = i;
                p.m_hrefpos[p.m_manifest.at(i).m_href] = i;
            }
        }
    }
    UpdateText(p);
}


void OPFResource::RemoveResource(const Resource *resource)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    if (p.m_manifest.isEmpty()) return;
    QString href = Utility::URLEncodePath(GetRelativePathToResource(resource));
    int pos = p.m_hrefpos.value(href, -1);
    QString item_id = "";

    // Delete the meta tag for cover images before deleting the manifest entry
    if (resource->Type() == Resource::ImageResourceType) {
      RemoveCoverMetaForImage(resource, p);
    }
    if (pos > -1) {
        item_id = p.m_manifest.at(pos).m_id;
    }
    if (resource->Type() == Resource::HTMLResourceType) {
        for (int i=0; i < p.m_spine.count(); ++i) {
            QString idref = p.m_spine.at(i).m_idref;
            if (idref == item_id) {
                p.m_spine.removeAt(i);
                break;
            }
        }
        RemoveAllGuideReferencesForResource(resource, p);
        QString version = GetEpubVersion();
        if (version.startsWith('3')) {
            NavProcessor navproc(GetNavResource());
            navproc.RemoveAllLandmarksForResource(resource);
        }
    }
    if (pos > -1) {
        p.m_manifest.removeAt(pos);
        // rebuild the maps since updating them item by item would be slower
        p.m_idpos.clear();
        p.m_hrefpos.clear();
        for (int i=0; i < p.m_manifest.count(); ++i) {
            p.m_idpos[p.m_manifest.at(i).m_id] = i;
            p.m_hrefpos[p.m_manifest.at(i).m_href] = i;
        }
    }
    UpdateText(p);
}


void OPFResource::ClearSemanticCodesInGuide()
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    foreach(GuideEntry ge, p.m_guide) {
        p.m_guide.removeAt(0);
    }
    UpdateText(p);
}


void OPFResource::AddGuideSemanticCode(HTMLResource *html_resource, QString new_code, bool toggle, QString tgt_id)
{
    //first get primary book language
    QString lang = GetPrimaryBookLanguage();
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    QString current_code = GetGuideSemanticCodeForResource(html_resource, p, tgt_id);

    if ((current_code != new_code) || !toggle) {
        RemoveDuplicateGuideCodes(new_code, p);
        SetGuideSemanticCodeForResource(new_code, html_resource, p, lang, tgt_id);
    } else {
        // If the current code is the same as the new one,
        // we toggle it off.
        RemoveGuideReferenceForResource(html_resource, p, tgt_id);
    }
    UpdateText(p);
}

QString OPFResource::GetGuideSemanticCodeForResource(const Resource *resource, const OPFParser &p, QString tgt_id) const
{
    QString gtype;
    int pos = GetGuideReferenceForResourcePos(resource, p, tgt_id);
    if (pos > -1) {
        GuideEntry ge = p.m_guide.at(pos);
        gtype = ge.m_type;
    }
    return gtype;
}

int OPFResource::GetGuideReferenceForResourcePos(const Resource *resource, const OPFParser &p, QString tgt_id) const
{
    QString href_to_resource_from_opf = Utility::URLEncodePath(GetRelativePathToResource(resource));
    for (int i=0; i < p.m_guide.count(); ++i) {
        GuideEntry ge = p.m_guide.at(i);
        QString href = ge.m_href;
        if (!tgt_id.isEmpty()){
            href_to_resource_from_opf = href_to_resource_from_opf + "#" + tgt_id;
        }
        if (href == href_to_resource_from_opf) {
            return i;
        }
    }
    return -1;
}

void OPFResource::RemoveDuplicateGuideCodes(QString code, OPFParser& p)
{
    // Industry best practice is to have only one
    // <guide> reference type instance per xhtml file.
    // For NoType, there is nothing to remove.
    if (code.isEmpty()) return;
    if (p.m_guide.isEmpty()) return;

    // build up the list to be deleted in reverse order
    QList<int> dellist;
    for (int i = p.m_guide.count() - 1; i >= 0; --i) {
        GuideEntry ge = p.m_guide.at(i);
        QString gtype = ge.m_type;
        if (gtype == code) {
            dellist.append(i);
        }
    }
    // remove them from the list in reverse order
    foreach(int index, dellist) {
        p.m_guide.removeAt(index);
    }
}

void OPFResource::RemoveGuideReferenceForResource(const Resource *resource, OPFParser& p, QString tgt_id)
{
    if (p.m_guide.isEmpty()) return;
    int pos = GetGuideReferenceForResourcePos(resource, p, tgt_id);
    while((pos > -1) && (!p.m_guide.isEmpty())) {
        p.m_guide.removeAt(pos);
        pos = GetGuideReferenceForResourcePos(resource, p, tgt_id);
    }
}

QStringList OPFResource::GetAllGuideInfoByBookPath() const
{
    QStringList guide_info;
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);    
    if (p.m_guide.isEmpty()) return guide_info;
    for (int i=0; i < p.m_guide.count(); ++i) {
        QString rec;
        GuideEntry ge = p.m_guide.at(i);
        QString href = ge.m_href;
        QString frag = "";
        QStringList parts = href.split('#', Qt::KeepEmptyParts);
        QString bkpath = Utility::buildBookPath(Utility::URLDecodePath(parts.at(0)), GetFolder());
        if (parts.size() > 1) frag = parts.at(1);
        rec = bkpath + _RS + frag + _RS + ge.m_type + _RS + ge.m_title;
        guide_info << rec;
    }
    return guide_info;
}

void OPFResource::RemoveAllGuideReferencesForResource(const Resource *resource, OPFParser& p)
{
    // if guide hrefs use fragments, the same resource may be there in multiple
    // guide entries.  Since resource being deleted, remove them all
    if (p.m_guide.isEmpty()) return;
    QString href_to_resource_from_opf = Utility::URLEncodePath(GetRelativePathToResource(resource));
    QList<int> positions_to_delete;
    for (int i=0; i < p.m_guide.count(); ++i) {
        GuideEntry ge = p.m_guide.at(i);
        QString href = ge.m_href;
        QStringList parts = href.split('#', Qt::KeepEmptyParts);
        if (parts.at(0) == href_to_resource_from_opf) {
            positions_to_delete << i;
        }
    }
    // handle deletions in reverse order to maintain position info
    while(positions_to_delete.size() > 0) {
        int pos = positions_to_delete.takeLast();
        p.m_guide.removeAt(pos);
    }
}


void OPFResource::UpdateGuideFragments(QHash<QString,QString> &idupdates)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    for(int c=0; c < p.m_guide.size(); c++) {
        GuideEntry ge = p.m_guide.at(c);
        QString href = ge.m_href;
        std::pair<QString, QString> parts = Utility::parseRelativeHREF(href);
        QString apath = Utility::URLDecodePath(parts.first);
        QString bkpath = Utility::buildBookPath(apath, GetFolder());
        QString key = bkpath + parts.second;
        QString newid = idupdates.value(key,"");
        if (!newid.isEmpty()) {
            // found a fragment id needing to be updated
            parts.second = "#" + newid;
            href = Utility::buildRelativeHREF(apath, parts.second);
            ge.m_href = href;
            p.m_guide[c] = ge;
        }
    }
    UpdateText(p);
}


// first merged resource in list is the sink resource
void OPFResource::UpdateGuideAfterMerge(QList<Resource*> &merged_resources, QHash<QString,QString> &section_id_map)
{
    if (merged_resources.isEmpty() || merged_resources.size() < 2) return;
    Resource* sink_resource = merged_resources.at(0);
    QString sink_bookpath = sink_resource->GetRelativePath();
    QStringList merged_bookpaths;
    for (int i=1; i < merged_resources.size(); i++) {
        Resource* res = merged_resources.at(i);
        merged_bookpaths << res->GetRelativePath();
    }
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    for(int c=0; c < p.m_guide.size(); c++) {
        GuideEntry ge = p.m_guide.at(c);
        QString href = ge.m_href;
        std::pair<QString, QString> parts = Utility::parseRelativeHREF(href);
        QString apath = Utility::URLDecodePath(parts.first);
        QString bkpath = Utility::buildBookPath(apath, GetFolder());
        if (merged_bookpaths.contains(bkpath)) {
            // need to redirect this bookpath to destination bookpath
            // handle nay redirect to new injected section fragments
            if (parts.second.isEmpty()) {
                if (section_id_map.contains(bkpath)) {
                    parts.second = "#" + section_id_map[bkpath];
                }
            }
            apath = Utility::buildRelativePath(GetRelativePath(), sink_bookpath);
            href = Utility::buildRelativeHREF(apath, parts.second);
            ge.m_href = href;
            p.m_guide[c] = ge;
        }
    }
    UpdateText(p);
}

void OPFResource::SetGuideSemanticCodeForResource(QString code, const Resource *resource,
                                                  OPFParser& p, const QString &lang, QString tgt_id)
{
    if (code.isEmpty()) return;
    int pos = GetGuideReferenceForResourcePos(resource, p, tgt_id);
    QString title = GuideItems::instance()->GetTitle(code, lang);
    if (pos > -1) {
        GuideEntry ge = p.m_guide.at(pos);
        ge.m_type = code;
        ge.m_title = title;
        p.m_guide.replace(pos, ge);
    } else {
        GuideEntry ge;
        ge.m_type = code;
        ge.m_title = title;
        QString href = Utility::URLEncodePath(GetRelativePathToResource(resource));
        if (!tgt_id.isEmpty()) {
            href = href + "#" + tgt_id;
        }
        ge.m_href = href; 
        p.m_guide.append(ge);
    }
}


QString OPFResource::GetGuideSemanticCodeForResource(const Resource *resource, QString tgt_id) const
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    return GetGuideSemanticCodeForResource(resource, p, tgt_id);
}


QString OPFResource::GetGuideSemanticNameForResource(Resource *resource, QString tgt_id)
{
    return GuideItems::instance()->GetName(GetGuideSemanticCodeForResource(resource, tgt_id));
}


// returns a hash of bookpath to a list of all semantic codes that exist in that file
QHash <QString, QStringList>  OPFResource::GetSemanticCodeForPaths()
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);

    QHash <QString, QStringList> semantic_codes;
    foreach(GuideEntry ge, p.m_guide) {
        QString href = ge.m_href;
        QStringList parts = href.split('#', Qt::KeepEmptyParts);
        QString apath = Utility::URLDecodePath(parts.at(0));
        QString bkpath = Utility::buildBookPath(apath, GetFolder());
        QString gtype = ge.m_type;
        QStringList codes;
        if (semantic_codes.contains(bkpath)) {
            codes = semantic_codes[bkpath];
        }
        codes << gtype;
        semantic_codes[bkpath] = codes;
    }
    return semantic_codes;
}


// returns a hash of bookpath to a list of all semantic names that exist in that file
QHash <QString, QStringList>  OPFResource::GetGuideSemanticNameForPaths()
{
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);

    QHash <QString, QStringList> semantic_types;
    foreach(GuideEntry ge, p.m_guide) {
        QString href = ge.m_href;
        QStringList parts = href.split('#', Qt::KeepEmptyParts);
        QString gtype = ge.m_type;
        QString apath = Utility::URLDecodePath(parts.at(0));
        QString bkpath = Utility::buildBookPath(apath, GetFolder());
        QStringList names;
        if (semantic_types.contains(bkpath)) {
            names = semantic_types[bkpath];
        }
        names << GuideItems::instance()->GetName(gtype);
        semantic_types[bkpath] = names;
    }

    // Cover image semantics don't use reference
    int pos  = GetCoverMeta(p);
    if (pos > -1) {
        MetaEntry me = p.m_metadata.at(pos);
        QString cover_id = me.m_atts.value(QString("content"),QString(""));
        ManifestEntry man = p.m_manifest.at(p.m_idpos[cover_id]);
        QString apath = Utility::URLDecodePath(man.m_href);
        QString bkpath = Utility::buildBookPath(apath, GetFolder());
        QStringList al;
        al << GuideItems::instance()->GetName("cover");
        semantic_types[bkpath] = al;
    }
    return semantic_types;
}


void OPFResource::SetResourceAsCoverImage(ImageResource *image_resource)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    QString resource_id = GetResourceManifestID(image_resource, p);

    // First deal with any previous covers by removing 
    // related metadata and manifest properties
    QString old_cover_resource_id;
    int pos = GetCoverMeta(p);
    if (pos > -1) {
        MetaEntry me = p.m_metadata.at(pos);
        old_cover_resource_id = me.m_atts.value(QString("content"),QString(""));
        p.m_metadata.removeAt(pos);
    }
    if (!old_cover_resource_id.isEmpty()) {
        if (p.m_package.m_version.startsWith("3")) {
            RemoveCoverImageProperty(old_cover_resource_id, p);
        }
    }

    // Now add in new metadata and manifest properties
    AddCoverMetaForImage(image_resource, p);
    if (p.m_package.m_version.startsWith("3")) {
        AddCoverImageProperty(resource_id, p);
    }
    UpdateText(p);
}


// note: under epub3 spine elements may have page properties set, so simply clearing the
// spine will lose these attributes.  We should try to keep as much of the spine properties
// and linear attributes as we can.  Either that or make the HTML Resource remember its own
// spine page properties, linear attribute, etc

void OPFResource::UpdateSpineOrder(const QList<::HTMLResource *> html_files)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    QList<SpineEntry> new_spine;
    foreach(HTMLResource * html_resource, html_files) {
        const Resource *resource = static_cast<const Resource *>(html_resource);
        QString id = GetResourceManifestID(resource, p);
        int found = -1;
        for (int i = 0; i < p.m_spine.count(); ++i) {
           SpineEntry se = p.m_spine.at(i);
           if (se.m_idref == id) {
               found = i;
               break;
           }
        }
        if (found > -1) {
            new_spine.append(p.m_spine.at(found));
        } else {
            SpineEntry se;
            se.m_idref = id;
            new_spine.append(se);
        }
    }
    p.m_spine.clear();
    p.m_spine = new_spine;
    UpdateText(p);
}


void OPFResource::ResourceRenamed(const Resource *resource, QString old_full_path)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    // first convert old_full_path to old_bkpath
    QString old_bkpath = old_full_path.right(old_full_path.length() - GetFullPathToBookFolder().length() - 1);
    QString old_href = Utility::URLEncodePath(Utility::buildRelativePath(GetRelativePath(), old_bkpath));
    QString old_id;
    QString new_id;
    for (int i=0; i < p.m_manifest.count(); ++i) {
        QString href = p.m_manifest.at(i).m_href;
        if (href == old_href) {
            ManifestEntry me = p.m_manifest.at(i);
            QString old_me_href = me.m_href;
            me.m_href = Utility::URLEncodePath(GetRelativePathToResource(resource));
            old_id = me.m_id;
            p.m_idpos.remove(old_id);
            new_id = GetUniqueID(GetValidID(resource->Filename()),p);
            me.m_id = new_id;
            p.m_idpos[new_id] = i;
            p.m_hrefpos.remove(old_me_href);
            p.m_hrefpos[me.m_href] = i;
            p.m_manifest.replace(i, me);
            break;
        }
    }
    for (int i=0; i < p.m_spine.count(); ++i) {
        QString idref = p.m_spine.at(i).m_idref;
        if (idref == old_id) {
            SpineEntry se = p.m_spine.at(i);
            se.m_idref = new_id;
            p.m_spine.replace(i, se);
            break;
        }
    }
    if (resource->Type() == Resource::NCXResourceType) {
        // handle updating the ncx id on the spine if ncx renamed
        QString ncx_id = p.m_spineattr.m_atts.value(QString("toc"),"");
        if (new_id != ncx_id) {
            p.m_spineattr.m_atts[QString("toc")] = new_id;
        }
    }
    if (resource->Type() == Resource::ImageResourceType) {
        // Change meta entry for cover if necessary
        // Check using IDs since file is already renamed
        if (IsCoverImageCheck(old_id, p)) {
            // Add will automatically replace an existing id
            // Assumes only one cover but removing duplicates
            // can cause timing issues
            AddCoverMetaForImage(resource, p);
        }
    }
    UpdateText(p);
}


void OPFResource::ResourceMoved(const Resource *resource, QString old_full_path)
{
    QWriteLocker locker(&GetLock());
    // QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    QString source = GetText();
    OPFParser p;
    p.parse(source);
    // first convert old_full_path to old_bkpath
    QString old_bkpath = old_full_path.right(old_full_path.length() - GetFullPathToBookFolder().length() - 1);
    QString old_href = Utility::URLEncodePath(Utility::buildRelativePath(GetRelativePath(), old_bkpath));
    // a move should not impact the id so leave the old unique manifest id unchanged
    for (int i=0; i < p.m_manifest.count(); ++i) {
        QString href = p.m_manifest.at(i).m_href;
        if (href == old_href) {
            ManifestEntry me = p.m_manifest.at(i);
            QString old_me_href = me.m_href;
            me.m_href = Utility::URLEncodePath(GetRelativePathToResource(resource));
            p.m_idpos[me.m_id] = i;
            p.m_hrefpos.remove(old_me_href);
            p.m_hrefpos[me.m_href] = i;
            p.m_manifest.replace(i, me);
            break;
        }
    }
    UpdateText(p);
}


void OPFResource::BulkResourcesMoved(const QHash<QString, Resource *> movedDict)
{
    QWriteLocker locker(&GetLock());
    QString opf_start_dir = Utility::startingDir(GetRelativePath());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);

    // a move should not impact the id so leave the old unique manifest id unchanged
    for (int i=0; i < p.m_manifest.count(); ++i) {
        QString href = p.m_manifest.at(i).m_href;
        QString bookpath = Utility::buildBookPath(href, opf_start_dir);
        if (movedDict.contains(bookpath)) {
            Resource * resource = movedDict[bookpath];
            ManifestEntry me = p.m_manifest.at(i);
            QString old_me_href = me.m_href;
            me.m_href = Utility::URLEncodePath(GetRelativePathToResource(resource));
            p.m_idpos[me.m_id] = i;
            p.m_hrefpos.remove(old_me_href);
            p.m_hrefpos[me.m_href] = i;
            p.m_manifest.replace(i, me);
        }
    }
    UpdateText(p);
}


void OPFResource::BulkResourcesRenamed(const QHash<QString, Resource *> renamedDict)
{
    QWriteLocker locker(&GetLock());
    QString opf_start_dir = Utility::startingDir(GetRelativePath());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);

    // a rename should not impact the id so leave the old unique manifest id unchanged
    for (int i=0; i < p.m_manifest.count(); ++i) {
        QString href = p.m_manifest.at(i).m_href;
        QString bookpath = Utility::buildBookPath(href, opf_start_dir);
        if (renamedDict.contains(bookpath)) {
            Resource * resource = renamedDict[bookpath];
            ManifestEntry me = p.m_manifest.at(i);
            QString old_me_href = me.m_href;
            me.m_href = Utility::URLEncodePath(GetRelativePathToResource(resource));
            p.m_idpos[me.m_id] = i;
            p.m_hrefpos.remove(old_me_href);
            p.m_hrefpos[me.m_href] = i;
            p.m_manifest.replace(i, me);
        }
    }
    UpdateText(p);
}


int OPFResource::GetCoverMeta(const OPFParser& p) const
{
    for (int i = 0; i < p.m_metadata.count(); ++i) {
        MetaEntry me = p.m_metadata.at(i);
        if ((me.m_name == "meta") && (me.m_atts.contains(QString("name")))) {
            QString name = me.m_atts[QString("name")];
            if (name == "cover") {
                return i;
            }
        }
    }
    return -1;
}


int OPFResource::GetMainIdentifier(const OPFParser& p) const
{
    QString unique_identifier = p.m_package.m_uniqueid;
    for (int i=0; i < p.m_metadata.count(); ++i) {
        MetaEntry me = p.m_metadata.at(i);
        if (me.m_name == "dc:identifier") {
            QString id = me.m_atts.value("id", "");
            if (id == unique_identifier) {
                return i;
            }
        }
    }
    return -1;
}

QHash<QString, Resource*>OPFResource::GetManifestIDResourceMapping(const QList<Resource *> &resources,
                               const OPFParser &p)
{
    QHash<QString, Resource*> id_mapping;
    foreach(Resource * resource, resources) {
        QString href_path = Utility::URLEncodePath(GetRelativePathToResource(resource));
        int pos = p.m_hrefpos.value(href_path,-1);
        if (pos > -1) { 
            id_mapping[ p.m_manifest.at(pos).m_id ] = resource;
        }
    }
    return id_mapping;
}


QString OPFResource::GetResourceManifestID(const Resource *resource, const OPFParser& p) const
{
    QString href_path = Utility::URLEncodePath(GetRelativePathToResource(resource));
    int pos = p.m_hrefpos.value(href_path,-1);
    if (pos > -1) { 
        return QString(p.m_manifest.at(pos).m_id); 
    }
    qDebug() << "GetResourceMainfestID returning null" << href_path;
    return QString();
}


QHash<Resource *, QString> OPFResource::GetResourceManifestIDMapping(const QList<Resource *> &resources, 
                                                                     const OPFParser& p)
{
    QHash<Resource *, QString> id_mapping;
    QStringList keys = p.m_hrefpos.keys();
    foreach(Resource * resource, resources) {
        QString href_path = Utility::URLEncodePath(GetRelativePathToResource(resource));
        // empty relative path from the OPF is the OPF which will not have a manifest entry
        if (!href_path.isEmpty()) {
            int pos = p.m_hrefpos.value(href_path,-1);
            if (pos > -1) { 
                id_mapping[ resource ] = p.m_manifest.at(pos).m_id;
            } else {
                qDebug() << "GetResourceMainifestIDMapping no map for" << href_path;
            }
        }
    }
    return id_mapping;
}


void OPFResource::RemoveDCElements(OPFParser& p)
{
    int pos = GetMainIdentifier(p);
    // build list to be delted in reverse order
    QList<int> dellist;
    int n = p.m_metadata.count();
    for (int i = n-1; i >= 0; --i) {
        MetaEntry me = p.m_metadata.at(i);
        if (me.m_name.startsWith("dc:")) {
            if (i != pos) {
               dellist.append(i);
            }
        }
    }
    // delete the MetaEntries in reverse order to not mess up indexes
    foreach(int index, dellist) {
        p.m_metadata.removeAt(index);
    }
}


void OPFResource::WriteSimpleMetadata(const QString &metaname, const QString &metavalue, OPFParser& p)
{
    MetaEntry me;
    me.m_name = QString("dc:") + metaname;
    me.m_content = metavalue.toHtmlEscaped();
    p.m_metadata.append(me);
}


void OPFResource::WriteIdentifier(const QString &metaname, const QString &metavalue, OPFParser& p)
{
    int pos = GetMainIdentifier(p);
    if (pos > -1) {
        MetaEntry me = p.m_metadata.at(pos);
        QString scheme = me.m_atts.value(QString("scheme"),QString(""));
    // epub3 no longer uses the scheme attribute
    if (scheme.isEmpty() && me.m_content.startsWith("urn:uuid:")) scheme="UUID";
        if ((metavalue == me.m_content) && (metaname == scheme)) {
            return;
        }
    }
    QString epubversion = GetEpubVersion();
    MetaEntry me;
    me.m_name = QString("dc:identifier");
    // under the latest epub3 spec "scheme" is no longer an allowed attribute of dc:identifier
    if (epubversion.startsWith('2')) {
        me.m_atts[QString("opf:scheme")] = metaname;
    }
    if (metaname.toLower() == "uuid" && !metavalue.contains("urn:uuid:")) {
        me.m_content = QString("urn:uuid:")  + metavalue;
    } else {
        me.m_content = metavalue;
    }
    p.m_metadata.append(me);
}

QString OPFResource::AddModificationDateMeta()
{
    QString datetime;
    QDateTime local(QDateTime::currentDateTime());
#if QT_VERSION >= QT_VERSION_CHECK(6,5,0)
    local.setTimeZone(QTimeZone::UTC);
#else
    local.setTimeSpec(Qt::UTC);
#endif
    datetime = local.toString(Qt::ISODate);

    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);

    QString epubversion = GetEpubVersion();
    if (epubversion.startsWith('3')) {

        // epub 3 set dcterms:modified date time in ISO 8601 format
        // if an entry exists, update it
        for (int i=0; i < p.m_metadata.count(); ++i) {
            MetaEntry me = p.m_metadata.at(i);
            if (me.m_name == QString("meta")) {
                QString property = me.m_atts.value(QString("property"), QString(""));
                if (property == QString("dcterms:modified")) {
                    me.m_content = datetime;
                    p.m_metadata.replace(i, me);
                    UpdateText(p);
                    return datetime;
                }
            }
        }
        // otherwize create a new entry
        MetaEntry me;
        me.m_name = QString("meta");
        me.m_content = datetime;
        me.m_atts["property"]="dcterms:modified";
        p.m_metadata.append(me);
        UpdateText(p);
        return datetime;
    }   
    // epub 2 version 
    QString date;
    QDate d = QDate::currentDate();
    // We can't use QDate.toString() because it will take into account the locale. Which mean we may not get Arabic 
    // numerals if the local uses it's own numbering system. So we use this instead to ensure we get a valid date per
    // the epub spec.
    QTextStream(&date) << d.year() << "-" << (d.month() < 10 ? "0" : "") << d.month() << "-" << (d.day() < 10 ? "0" : "") << d.day();
    // if an entry exists, update it
    for (int i=0; i < p.m_metadata.count(); ++i) {
        MetaEntry me = p.m_metadata.at(i);
        if (me.m_name == QString("dc:date")) {
            QString etype = me.m_atts.value(QString("opf:event"), QString(""));
            if (etype == QString("modification")) {
                me.m_content = date;
                p.m_metadata.replace(i, me);
                UpdateText(p);
                return datetime;
            }
            
        }
    }
    // otherwize create a new entry
    MetaEntry me;
    me.m_name = QString("dc:date");
    me.m_content = date;
    me.m_atts["xmlns:opf"]="http://www.idpf.org/2007/opf";
    me.m_atts[QString("opf:event")] = QString("modification");
    p.m_metadata.append(me);
    UpdateText(p);
    return datetime;
}


QString OPFResource::GetOPFDefaultText(const QString &version)
{
    SettingsStore ss;
    QString defaultLanguage = ss.defaultMetadataLang();
    if (version.startsWith('2')) {
        return TEMPLATE_TEXT.arg(Utility::CreateUUID()).arg(defaultLanguage).arg(tr("[Title here]"));
    }
    // epub 3 set dcterms:modified date time in ISO 8601 format
    QDateTime local(QDateTime::currentDateTime());
#if QT_VERSION >= QT_VERSION_CHECK(6,5,0)
    local.setTimeZone(QTimeZone::UTC);
#else
    local.setTimeSpec(Qt::UTC);
#endif
    QString datetime = local.toString(Qt::ISODate);
    return TEMPLATE3_TEXT.arg(Utility::CreateUUID()).arg(defaultLanguage).arg(tr("[Main title here]")).arg(datetime);
}


void OPFResource::FillWithDefaultText(const QString &version)
{
    QString epubversion = version;
    if (epubversion.isEmpty()) {
        SettingsStore ss;
        epubversion = ss.defaultVersion();
    }
    SetEpubVersion(epubversion);
    SetText(GetOPFDefaultText(epubversion));
}


QString OPFResource::GetUniqueID(const QString &preferred_id, const OPFParser& p) const
{
    if (p.m_idpos.contains(preferred_id)) {
        return QString("x").append(Utility::CreateUUID());
    }
    return preferred_id;
}


QString OPFResource::GetResourceMimetype(const Resource *resource) const
{
    QString mimetype = resource->GetMediaType();
    QString absolute_file_path = resource->GetFullPath();
    if (mimetype.isEmpty()) {
        QString extension = QFileInfo(absolute_file_path).suffix().toLower();
        mimetype = MediaTypes::instance()->GetMediaTypeFromExtension(extension, "");
    }
    if (mimetype.isEmpty()) {
        mimetype = MediaTypes::instance()->GetFileDataMimeType(absolute_file_path, "");
    }
    if (mimetype == "application/xml") {
        mimetype = MediaTypes::instance()->GetMediaTypeFromXML(absolute_file_path, "application/xml");
    }
    return mimetype;
}


void OPFResource::UpdateManifestMediaTypes(const QList<Resource*> resources)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    foreach(Resource* resource, resources) {
        // QString absolute_file_path = resource->GetFullPath();
        // QString extension = QFileInfo(absolute_file_path).suffix().toLower();
        // QString ext_mimetype = MediaTypes::instance()->GetMediaTypeFromExtension(extension, "");
        // QString data_mimetype = MediaTypes::instance()->GetFileDataMimeType(absolute_file_path, "");
        QString manifest_mimetype;
        QString resource_mimetype = GetResourceMimetype(resource);
        QString href = Utility::URLEncodePath(GetRelativePathToResource(resource));
        int pos = p.m_hrefpos.value(href, -1);
        if ((pos >= 0) && (pos < p.m_manifest.count())) {
            ManifestEntry me = p.m_manifest.at(pos);
            manifest_mimetype = me.m_mtype;
            // qDebug() << "    ";
            // qDebug() << "resource name:     " << resource->GetRelativePath();
            // qDebug() << "resource mimetype: " << resource_mimetype;
            // qDebug() << "ext mimetype:      " << ext_mimetype;
            // qDebug() << "data mimetype:     " << data_mimetype;
            // qDebug() << "manifest_mimetype: " << manifest_mimetype;
            if (manifest_mimetype != resource_mimetype) {
                me.m_mtype = resource_mimetype;
                p.m_manifest.replace(pos, me);
            }
        }
    }
    UpdateText(p);
}


void OPFResource::UpdateText(const OPFParser &p)
{
    TextResource::SetText(p.convert_to_xml());
}


QString OPFResource::ValidatePackageVersion(const QString& source)
{
    QString newsource = source;
    QString orig_version = GetEpubVersion();
    QRegularExpression pkgversion_search(PKG_VERSION, QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch mo = pkgversion_search.match(newsource);
    if (mo.hasMatch()) {
        QString version = mo.captured(1);
        if (version != orig_version) {
            newsource.replace(mo.capturedStart(1), mo.capturedLength(1), orig_version);
            if (!m_WarnedAboutVersion && !version.startsWith('1')) {
                Utility::DisplayStdWarningDialog("Changing package version inside Sigil is not supported", 
                                                 "Use an appropriate output plugin to make the initial conversion");
                m_WarnedAboutVersion = true;
            }
        }
    }
    return newsource;
}


void OPFResource::UpdateManifestProperties(const QList<Resource*> resources)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    if (p.m_package.m_version != "3.0") {
        return;
    }
    foreach(Resource* resource, resources) {
        const HTMLResource* html_resource = static_cast<const HTMLResource *>(resource);
        QString href = Utility::URLEncodePath(GetRelativePathToResource(html_resource));
        int pos = p.m_hrefpos.value(href, -1);
        if ((pos >= 0) && (pos < p.m_manifest.count())) {
            ManifestEntry me = p.m_manifest.at(pos);
            QStringList properties = html_resource->GetManifestProperties();
            // The nav must not lose the nav property
            if (html_resource == m_NavResource) {
                if (!properties.contains("nav")) {
                    properties << "nav";
                }
            }
            me.m_atts.remove("properties");
            if (properties.count() > 0) {
                me.m_atts["properties"] = properties.join(QString(" "));
            }
            p.m_manifest.replace(pos, me);
        }
    }
    // now add the cover-image properties
    int metapos  = GetCoverMeta(p);
    if (metapos > -1) {
        MetaEntry cmeta = p.m_metadata.at(metapos);
        QString cover_id = cmeta.m_atts.value(QString("content"),QString(""));
        if (!cover_id.isEmpty()) {
            int pos = p.m_idpos.value(cover_id, -1);
            if (pos >= 0 ) {
                ManifestEntry me = p.m_manifest.at(p.m_idpos[cover_id]);
                QString props = me.m_atts.value("properties", "");
                if (!props.contains("cover-image")) {
                    props = props + " cover-image";
                }
                props = props.simplified();
                me.m_atts.remove("properties");
                me.m_atts["properties"] = props;
                p.m_manifest.replace(pos, me);
            }
        }
    }
    UpdateText(p);
}


QString OPFResource::GetManifestPropertiesForResource(const Resource * resource)
{
    QString properties;
    if (!resource) return properties;
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    if (!p.m_package.m_version.startsWith("3")) {
        return properties;
    }
    QString href = Utility::URLEncodePath(GetRelativePathToResource(resource));
    int pos = p.m_hrefpos.value(href, -1);
    if ((pos >= 0) && (pos < p.m_manifest.count())) {
        ManifestEntry me = p.m_manifest.at(pos);
        properties = me.m_atts.value("properties","");
    }
    return properties;
}


QHash <QString, QString>  OPFResource::GetManifestPropertiesForPaths()
{
    QHash <QString, QString> manifest_properties_all;
    QString version = GetEpubVersion();
    if (!version.startsWith('3')) {
        return manifest_properties_all;
    }
    QReadLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    foreach(ManifestEntry me, p.m_manifest) {
        QString apath = Utility::URLDecodePath(me.m_href);
        if (me.m_atts.contains("properties")){
            QString properties = me.m_atts["properties"];
            apath = Utility::buildBookPath(apath, GetFolder());
            manifest_properties_all[apath] = properties;
        }
    }
    return manifest_properties_all;
}


HTMLResource * OPFResource::GetNavResource()const
{
    return m_NavResource;
}


void OPFResource::SetNavResource(HTMLResource * nav_resource)
{
    m_NavResource = nav_resource;
    // Make sure the proper nav property is set in the opf manifest
    // but do not overwrite any other existing properties
    if (m_NavResource) { 
        QWriteLocker locker(&GetLock());
        QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
        OPFParser p;
        p.parse(source);
        QString href = Utility::URLEncodePath(GetRelativePathToResource(m_NavResource));
        int pos = p.m_hrefpos.value(href, -1);
        if ((pos >= 0) && (pos < p.m_manifest.count())) {
            ManifestEntry me = p.m_manifest.at(pos);
            QString props = me.m_atts.value("properties", "");
            if (!props.contains("nav")) {
                props = props + " nav";
            }
            props = props.simplified();
            me.m_atts.remove("properties");
            me.m_atts["properties"] = props;
            p.m_manifest.replace(pos, me);
        }
        UpdateText(p);
    }
}


void OPFResource::SetItemRefLinear(Resource * resource, bool linear)
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    OPFParser p;
    p.parse(source);
    QString resource_href_path = Utility::URLEncodePath(GetRelativePathToResource(resource));
    int pos = p.m_hrefpos.value(resource_href_path, -1);
    QString item_id = "";
    if (pos > -1) {
        item_id = p.m_manifest.at(pos).m_id;
        if (resource->Type() == Resource::HTMLResourceType) {
            for (int i=0; i < p.m_spine.count(); ++i) {
                QString idref = p.m_spine.at(i).m_idref;
                if (idref == item_id) {
                    SpineEntry se = p.m_spine.at(i);
                    se.m_atts.remove(QString("linear"));
                    // default is linear = "yes"
                    if (!linear) {
                        se.m_atts[QString("linear")] = QString("no");
                    }
                    p.m_spine.replace(i, se);
                    break;
                }
            }
        }
        UpdateText(p);
    }
}


void OPFResource::RebaseManifestIDs()
{
    QWriteLocker locker(&GetLock());
    QString source = CleanSource::ProcessXML(GetText(),"application/oebps-package+xml");
    PythonRoutines pr;
    source = pr.RebaseManifestIDsInPython(source);
    TextResource::SetText(source);
}
