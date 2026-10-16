#include "DocumentIO.h"

#include "lemma/worksheet.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {

constexpr auto kFormat = "lemma-worksheet";
// 2: text regions; 3: plot regions. Older readers would silently drop them, so they must
// refuse the file.
constexpr int kVersion = 3;

} // namespace

namespace DocumentIO {

bool save(const lemma::Worksheet& worksheet, const QString& path, QString* error)
{
    QJsonArray regions;
    for (const lemma::Region& r : worksheet.regions()) {
        if (r.kind == lemma::Region::Kind::Plot) {
            regions.append(QJsonObject{
                {"type", "plot"},
                {"x", r.x},
                {"y", r.y},
                {"width", r.width},
                {"height", r.height},
                {"traces", QString::fromStdString(r.source)},
                {"xVariable", QString::fromStdString(r.axes.x)},
                {"xMin", QString::fromStdString(r.axes.xMin)},
                {"xMax", QString::fromStdString(r.axes.xMax)},
                {"yMin", QString::fromStdString(r.axes.yMin)},
                {"yMax", QString::fromStdString(r.axes.yMax)},
            });
        } else if (r.kind == lemma::Region::Kind::Text) {
            regions.append(QJsonObject{
                {"type", "text"},
                {"x", r.x},
                {"y", r.y},
                {"width", r.width},
                {"text", QString::fromStdString(r.source)},
            });
        } else {
            regions.append(QJsonObject{
                {"type", "math"},
                {"x", r.x},
                {"y", r.y},
                {"source", QString::fromStdString(r.source)},
            });
        }
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
        const QString type = r.value("type").toString();
        if (type == "math") {
            worksheet.addRegion(r.value("x").toDouble(), r.value("y").toDouble(),
                                r.value("source").toString().toStdString());
        } else if (type == "text") {
            worksheet.addTextRegion(r.value("x").toDouble(), r.value("y").toDouble(), r.value("width").toDouble(),
                                    r.value("text").toString().toStdString());
        } else if (type == "plot") {
            const int id = worksheet.addPlotRegion(r.value("x").toDouble(), r.value("y").toDouble(),
                                                   r.value("width").toDouble(), r.value("height").toDouble(),
                                                   r.value("traces").toString().toStdString());
            lemma::PlotAxes& axes = worksheet.region(id)->axes;
            axes.x = r.value("xVariable").toString().toStdString();
            axes.xMin = r.value("xMin").toString().toStdString();
            axes.xMax = r.value("xMax").toString().toStdString();
            axes.yMin = r.value("yMin").toString().toStdString();
            axes.yMax = r.value("yMax").toString().toStdString();
        }
    }
    worksheet.evaluate();
    return true;
}

} // namespace DocumentIO
