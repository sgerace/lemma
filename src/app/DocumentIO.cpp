#include "DocumentIO.h"

#include "lemma/worksheet.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {

constexpr auto kFormat = "lemma-worksheet";
constexpr int kVersion = 1;

} // namespace

namespace DocumentIO {

bool save(const lemma::Worksheet& worksheet, const QString& path, QString* error)
{
    QJsonArray regions;
    for (const lemma::Region& r : worksheet.regions()) {
        regions.append(QJsonObject{
            {"type", "math"},
            {"x", r.x},
            {"y", r.y},
            {"source", QString::fromStdString(r.source)},
        });
    }
    const QJsonObject root{
        {"format", kFormat},
        {"version", kVersion},
        {"regions", regions},
    };

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = file.errorString();
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}

bool load(lemma::Worksheet& worksheet, const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (doc.isNull()) {
        *error = parseError.errorString();
        return false;
    }

    const QJsonObject root = doc.object();
    if (root.value("format").toString() != kFormat) {
        *error = QObject::tr("This is not a Lemma worksheet.");
        return false;
    }
    if (root.value("version").toInt() > kVersion) {
        *error = QObject::tr("This worksheet was saved by a newer version of Lemma.");
        return false;
    }

    worksheet.clear();
    for (const QJsonValue& value : root.value("regions").toArray()) {
        const QJsonObject r = value.toObject();
        if (r.value("type").toString() == "math") {
            worksheet.addRegion(r.value("x").toDouble(), r.value("y").toDouble(),
                                r.value("source").toString().toStdString());
        }
    }
    worksheet.evaluate();
    return true;
}

} // namespace DocumentIO
