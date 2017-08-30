#include "dvirtualimagefileio.h"
#include "helper.h"
#include "dglobal.h"

#include <QDataStream>
#include <QCryptographicHash>

class DVirtualImageFileIOPrivate : public QSharedData
{
public:
    bool isValid = false;

    QFile file;

    quint8 version;

    struct FileInfo {
        FileInfo &operator=(const FileInfo &other) {
            index = other.index;
            name = other.name;
            start = other.start;
            end = other.end;

            return *this;
        }

        quint8 index;
        QString name;
        qint64 start;
        qint64 end;
    };

    QHash<QString, FileInfo> fileMap;
    QString openedFile;

    QStringList fileNameList() const;
    QVarLengthArray<FileInfo> fileList() const;

    static QMap<QString, DVirtualImageFileIOPrivate*> dMap;
};

QMap<QString, DVirtualImageFileIOPrivate*> DVirtualImageFileIOPrivate::dMap;

DVirtualImageFileIO::DVirtualImageFileIO(const QString &fileName)
{
    DVirtualImageFileIOPrivate *dd = DVirtualImageFileIOPrivate::dMap.value(fileName);

    if (!dd) {
        dd = new DVirtualImageFileIOPrivate();
        DVirtualImageFileIOPrivate::dMap[fileName] = dd;
    }

    d = dd;

    setFile(fileName);
}

DVirtualImageFileIO::~DVirtualImageFileIO()
{
    if (d->ref == 1)
        DVirtualImageFileIOPrivate::dMap.remove(d->file.fileName());

    close();
}

template<typename T>
T getData(QDataStream &stream)
{
    T t;

    stream >> t;

    return t;
}

bool DVirtualImageFileIO::setFile(const QString &fileName)
{
    if (d->file.isOpen()) {
        dCDebug("File %s already open", qPrintable(fileName));

        return false;
    }

    d->isValid = false;
    d->file.close();

    if (!fileName.endsWith(".dim"))
        return false;

    d->file.setFileName(fileName);

    if (!d->file.exists())
        return false;

    if (d->file.size() > 0) {
        if (d->file.size() < metaDataSize()) {
            dCDebug("Not a valid dim file");

            return false;
        }

        if (!d->file.open(QIODevice::ReadOnly)) {
            return false;
        }

        QDataStream stream(&d->file);

        stream.setVersion(QDataStream::Qt_5_6);

        if (getData<quint8>(stream) != 0xdd) {
            dCDebug("The 1dth character should be 0xdd");

            d->file.close();

            return false;
        }

        stream >> d->version;

        if (d->version != 1) {
            dCDebug("Unsupported version: %d", (int)d->version);

            d->file.close();

            return false;
        }

        quint8 file_count;

        stream >> file_count;

        if (d->file.size() < 3 + file_count * 80) {
            dCDebug("Not a valid dim file");

            d->file.close();

            return false;
        }

        for (quint8 i = 0; i < file_count; ++i) {
            if (getData<quint8>(stream) != 0xdd) {
                dCDebug("The %lldth character should be 0xdd", d->file.pos());

                d->file.close();

                return false;
            }

            DVirtualImageFileIOPrivate::FileInfo info;

            info.name = QString::fromUtf8(d->file.read(63));
            info.index = i;

            stream >> info.start;
            stream >> info.end;

            d->fileMap[info.name] = info;
        }

        const QByteArray &md5 = d->file.read(16);

        if (md5 != md5sum()) {
            dCDebug("MD5 check failed, file: %s, Is the file open in other application?", qPrintable(fileName));

            return false;
        }
    } else if (d->file.open(QIODevice::WriteOnly)) {
        d->file.resize(metaDataSize());
        d->file.putChar(0xdd);
        d->file.putChar(0x01);
        d->file.putChar(0x00);
        // init md5 sum
        uchar md5[16] = {0x21, 0x21, 0x5b, 0x9a, 0xf6, 0xd0, 0x5c, 0x31,
                        0xd8, 0xcd, 0x42, 0xbb, 0xca, 0x12, 0x97, 0x7f};
        d->file.write((char*)md5, 16);
    } else {
        return false;
    }

    d->file.close();
    d->isValid = true;

    return true;
}

bool DVirtualImageFileIO::setSize(qint64 size)
{
    return d->file.resize(size);
}

bool DVirtualImageFileIO::isValid() const
{
    return d->isValid;
}

bool DVirtualImageFileIO::existes(const QString &fileName) const
{
    return d->fileMap.contains(fileName);
}

bool DVirtualImageFileIO::isOpen(const QString &fileName) const
{
    return d->openedFile == fileName;
}

bool DVirtualImageFileIO::open(const QString &fileName, QIODevice::OpenMode openMode)
{
    if (d->file.isOpen() || !isValid())
        return false;

    if (openMode.testFlag(QIODevice::NotOpen))
        return false;

    if (openMode & (QIODevice::WriteOnly | QIODevice::Append)) {
        if (!isWritable(fileName)) {
            return false;
        }
    } else if (!existes(fileName)) {
        return false;
    }

    if (!existes(fileName)) {
        addFile(fileName);
    }

    if (!d->file.open(openMode | QIODevice::ReadOnly))
        return false;

    const DVirtualImageFileIOPrivate::FileInfo &info = d->fileMap.value(fileName);

    d->file.seek(info.start);
    d->openedFile = fileName;

    return true;
}

bool DVirtualImageFileIO::close()
{
    if (!d->file.isOpen())
        return false;

    const QFile::OpenMode open_mode = d->file.openMode();

    if (open_mode.testFlag(QFile::WriteOnly)) {
        if (!d->openedFile.isEmpty()) {
            const DVirtualImageFileIOPrivate::FileInfo &info = d->fileMap.value(d->openedFile);

            d->file.close();
            setSize(d->openedFile, info.end - info.start);
        } else {
            d->openedFile.clear();
        }

        if (!d->file.isOpen()) {
            if (!d->file.open(QIODevice::ReadWrite)) {
                return false;
            }
        }

        const QByteArray &md5 = md5sum();

        d->file.seek(validMetaDataSize());
        d->file.write(md5);
    }

    d->file.close();

    return d->file.error() == QFile::NoError;
}

qint64 DVirtualImageFileIO::pos() const
{
    if (d->openedFile.isEmpty())
        return -1;

    const DVirtualImageFileIOPrivate::FileInfo &info = d->fileMap.value(d->openedFile);

    qint64 pos = d->file.pos();

    if (pos < info.start || pos > info.end)
        return -1;

    return d->file.pos() - info.start;
}

bool DVirtualImageFileIO::seek(qint64 pos)
{
    if (pos < 0)
        return false;

    if (d->openedFile.isEmpty())
        return -1;

    const DVirtualImageFileIOPrivate::FileInfo &info = d->fileMap.value(d->openedFile);

    return d->file.seek(info.start + pos);
}

bool DVirtualImageFileIO::flush()
{
    return d->file.flush();
}

bool DVirtualImageFileIO::isSequential() const
{
    return d->file.isSequential();
}

QFile::Permissions DVirtualImageFileIO::permissions() const
{
    return d->file.permissions();
}

qint64 DVirtualImageFileIO::read(char *data, qint64 maxlen)
{
    maxlen = qMin(maxlen, d->fileMap.value(d->openedFile).end - d->file.pos());

    return d->file.read(data, maxlen);
}

qint64 DVirtualImageFileIO::write(const char *data, qint64 len)
{
    len = d->file.write(data, len);

    DVirtualImageFileIOPrivate::FileInfo &info = d->fileMap[d->openedFile];
    info.end = qMax(info.end, d->file.pos());

    return len;
}

qint64 DVirtualImageFileIO::size(const QString &fileName) const
{
    if (!d->fileMap.contains(fileName))
        return -1;

    const DVirtualImageFileIOPrivate::FileInfo &info = d->fileMap.value(fileName);

    return info.end - info.start;
}

qint64 DVirtualImageFileIO::start(const QString &fileName) const
{
    if (!d->fileMap.contains(fileName))
        return -1;

    const DVirtualImageFileIOPrivate::FileInfo &info = d->fileMap.value(fileName);

    return info.start;
}

qint64 DVirtualImageFileIO::end(const QString &fileName) const
{
    if (!d->fileMap.contains(fileName))
        return -1;

    const DVirtualImageFileIOPrivate::FileInfo &info = d->fileMap.value(fileName);

    return info.end;
}

bool DVirtualImageFileIO::setSize(const QString &fileName, qint64 size)
{
    if (!isWritable(fileName))
        return false;

    if (size < 0)
        return false;

    if (!d->file.open(QIODevice::ReadWrite))
        return false;

    d->fileMap[fileName].end = d->fileMap.value(fileName).start + size;
    d->file.seek(3 + d->fileMap.count() * 80 - 8);

    QDataStream stream(&d->file);

    stream.setVersion(QDataStream::Qt_5_6);
    stream << d->fileMap.value(fileName).end;

    d->file.close();

    return d->file.error() == QFile::NoError;
}

bool DVirtualImageFileIO::rename(const QString &from, const QString &to)
{
    if (!existes(from))
        return false;

    DVirtualImageFileIOPrivate::FileInfo info = d->fileMap.take(from);

    info.name = to;

    qint64 pos = d->file.pos();

    if (!d->file.seek(3 + info.index * 80 + 1))
        return false;

    const QByteArray &file_name = to.toUtf8();

    if (file_name.size() > 63) {
        dCDebug("File name length exceeds limit");

        return false;
    }

    d->file.write(file_name);

    if (file_name.size() < 63) {
        char empty_ch[63 - file_name.size()] = {0};

        d->file.write(empty_ch, 63 - file_name.size());
    }

    d->file.seek(pos);

    return true;
}

bool DVirtualImageFileIO::isWritable(const QString &fileName)
{
    if (!existes(fileName))
        return true;

    const DVirtualImageFileIOPrivate::FileInfo &info = d->fileMap.value(fileName);

    return info.index == d->fileMap.count() - 1;
}

int DVirtualImageFileIO::maxFileCount()
{
    return UINT8_MAX;
}

qint64 DVirtualImageFileIO::metaDataSize()
{
    return 24 * 1024;
}

qint64 DVirtualImageFileIO::validMetaDataSize() const
{
    return 3 + d->fileMap.count() * 80;
}

qint64 DVirtualImageFileIO::fileDataSize() const
{
    if (d->fileMap.isEmpty())
        return 0;

    qint64 max_end = 0;

    for (const DVirtualImageFileIOPrivate::FileInfo &info : d->fileMap) {
        max_end = qMax(max_end, info.end);
    }

    return max_end - metaDataSize();
}

qint64 DVirtualImageFileIO::writableDataSize() const
{
    return d->file.size() - fileDataSize() - metaDataSize();
}

QStringList DVirtualImageFileIO::fileList() const
{
    return d->fileNameList();
}

bool DVirtualImageFileIO::addFile(const QString &name)
{
    if (!d->file.open(QIODevice::ReadWrite)) {
        return false;
    }

    qint64 start = validMetaDataSize();

    d->file.seek(start);

    if (!d->file.putChar(0xdd)) {
        d->file.close();

        return false;
    }

    const QByteArray &file_name = name.toUtf8();

    if (file_name.size() > 63) {
        dCDebug("File name length exceeds limit");

        d->file.close();

        return false;
    }

    d->file.write(file_name);

    if (file_name.size() < 63) {
        char empty_ch[63 - file_name.size()] = {0};

        d->file.write(empty_ch, 63 - file_name.size());
    }

    DVirtualImageFileIOPrivate::FileInfo info;

    info.name = name;
    info.start = metaDataSize() + fileDataSize();
    info.end = info.start;
    info.index = d->fileMap.count();

    d->fileMap[name] = info;

    QDataStream stream(&d->file);

    stream.setVersion(QDataStream::Qt_5_6);
    stream << info.start;
    stream << info.end;

    d->file.seek(2);
    d->file.putChar(d->fileMap.count());
    d->file.close();

    return true;
}

QByteArray DVirtualImageFileIO::md5sum()
{
    if (!d->file.isOpen())
        return QByteArray();

    d->file.seek(0);

    QCryptographicHash md5(QCryptographicHash::Md5);

    md5.addData(d->file.read(validMetaDataSize()));

    for (const DVirtualImageFileIOPrivate::FileInfo &info : d->fileList()) {
        d->file.seek(info.start);

        while (d->file.pos() < info.end - 1024 * 1024 - 2) {
            quint16 block_index = 0;

            d->file.read((char*)(&block_index), sizeof(block_index));

            block_index %= 1024;

            if (!d->file.seek(d->file.pos() + block_index * 1024))
                break;

            md5.addData(d->file.read(1024));
        }

        md5.addData(d->file.read(info.end - d->file.pos()));
    }

    return md5.result();
}

QStringList DVirtualImageFileIOPrivate::fileNameList() const
{
    QStringList list;

    list.reserve(fileMap.size());

    while (list.count() < fileMap.count())
        list.append(QString());

    for (const FileInfo &info : fileMap) {
        list[info.index] = info.name;
    }

    return list;
}

QVarLengthArray<DVirtualImageFileIOPrivate::FileInfo> DVirtualImageFileIOPrivate::fileList() const
{
    QVarLengthArray<FileInfo> list;

    list.resize(fileMap.size());

    for (const FileInfo &info : fileMap) {
        list[info.index] = info;
    }

    return list;
}
