#include "DocumentIO.h"

#include "lemma/worksheet.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace {

constexpr auto kFormat = "lemma-worksheet";
// 2: text regions; 3: plot regions; 4: math regions store their tree, not linear text;
// 5: symbolic evaluations and equations. Lemma is pre-release, so older versions are
// refused rather than migrated.
constexpr int kVersion = 5;

using Kind = lemma::MathNode::Kind;

// A math tree as JSON (docs/editor-spec.md, section 11.1): nested objects with full kind
// names and named slots. Node ids aren't saved; loading assigns new ones.
QJsonObject toJson(const lemma::MathNode& n)
{
    const auto& c = n.children;
    switch (n.kind) {
    case Kind::Number:
        return {{"kind", "number"}, {"text", QString::fromStdString(n.text)}};
    case Kind::Identifier:
        return {{"kind", "identifier"}, {"text", QString::fromStdString(n.text)}};
    case Kind::Placeholder:
        return {{"kind", "placeholder"}};
    case Kind::Binary: {
        QJsonObject o{{"lhs", toJson(c[0])}, {"rhs", toJson(c[1])}};
        switch (n.op) {
        case '+': o["kind"] = "add"; break;
        case '-': o["kind"] = "subtract"; break;
        case '*':
            o["kind"] = "multiply";
            o["style"] = n.juxtaposed ? "juxtaposed" : "dot";
            break;
        default: o["kind"] = "missing-operator"; break;
        }
        return o;
    }
    case Kind::Negate:
        return {{"kind", "negate"}, {"operand", toJson(c[0])}};
    case Kind::Fraction:
        return {{"kind", "fraction"}, {"num", toJson(c[0])}, {"den", toJson(c[1])}};
    case Kind::Power:
        return {{"kind", "power"}, {"base", toJson(c[0])}, {"exp", toJson(c[1])}};
    case Kind::Radical:
        return {{"kind", "radical"}, {"radicand", toJson(c[0])}};
    case Kind::Group:
        return {{"kind", "group"}, {"contents", toJson(c[0])}};
    case Kind::Call: {
        QJsonArray args;
        for (std::size_t i = 1; i < c.size(); ++i) {
            args.append(toJson(c[i]));
        }
        return {{"kind", "call"}, {"name", toJson(c[0])}, {"args", args}};
    }
    case Kind::Definition:
        return {{"kind", "definition"}, {"lhs", toJson(c[0])}, {"rhs", toJson(c[1])}};
    case Kind::Evaluation:
        return {{"kind", "evaluation"}, {"expr", toJson(c[0])}, {"unit", toJson(c[1])}};
    case Kind::Symbolic: {
        // Results are recomputed on loading, not saved (symbolic spec, section 10).
        QJsonArray keywords;
        for (std::size_t i = 1; i < c.size(); ++i) {
            keywords.append(toJson(c[i]));
        }
        return {{"kind", "symbolic"}, {"expr", toJson(c[0])}, {"keywords", keywords}};
    }
    case Kind::Keyword: {
        QJsonArray args;
        for (std::size_t i = 1; i < c.size(); ++i) {
            args.append(toJson(c[i]));
        }
        return {{"kind", "keyword"}, {"name", toJson(c[0])}, {"args", args}};
    }
    case Kind::Equation:
        return {{"kind", "equation"}, {"lhs", toJson(c[0])}, {"rhs", toJson(c[1])}};
    }
    return {};
}

struct BadTree {};

// Where a node is, which decides what it may be: a statement only as the whole region,
// a keyword only in a symbolic evaluation's keyword slots, and an equation only as a
// whole slot.
enum class Place { Root, Expression, KeywordSlot, Argument, Other };

lemma::MathNode fromJson(const QJsonValue& value, Place place = Place::Other)
{
    const bool root = place == Place::Root;
    if (!value.isObject()) {
        throw BadTree{};
    }
    const QJsonObject o = value.toObject();
    const QString kind = o.value("kind").toString();
    const auto slot = [&](const char* name, Place p = Place::Other) { return fromJson(o.value(name), p); };
    const auto text = [&] {
        const std::string t = o.value("text").toString().toStdString();
        if (t.empty()) {
            throw BadTree{};
        }
        return t;
    };
    const auto binary = [&](char op, bool juxtaposed = false) {
        return lemma::MathNode::binary(op, slot("lhs"), slot("rhs"), juxtaposed);
    };
    if (kind == "number") return lemma::MathNode::number(text());
    if (kind == "identifier") return lemma::MathNode::identifier(text());
    if (kind == "placeholder") return lemma::MathNode();
    if (kind == "add") return binary('+');
    if (kind == "subtract") return binary('-');
    if (kind == "multiply") return binary('*', o.value("style").toString() == "juxtaposed");
    if (kind == "missing-operator") return binary(0);
    if (kind == "negate") return lemma::MathNode(Kind::Negate, {slot("operand")});
    if (kind == "fraction") return lemma::MathNode(Kind::Fraction, {slot("num"), slot("den")});
    if (kind == "power") return lemma::MathNode(Kind::Power, {slot("base"), slot("exp")});
    if (kind == "radical") return lemma::MathNode(Kind::Radical, {slot("radicand")});
    if (kind == "group") return lemma::MathNode(Kind::Group, {slot("contents")});
    if (kind == "call") {
        lemma::MathNode call(Kind::Call, {slot("name")});
        for (const QJsonValue& arg : o.value("args").toArray()) {
            call.children.push_back(fromJson(arg));
        }
        const Kind name = call.children[0].kind;
        if (call.children.size() < 2 || (name != Kind::Identifier && name != Kind::Placeholder)) {
            throw BadTree{};
        }
        return call;
    }
    // A definition or evaluation is only ever a whole region.
    if (root && kind == "definition") return lemma::MathNode(Kind::Definition, {slot("lhs"), slot("rhs")});
    if (root && kind == "evaluation") return lemma::MathNode(Kind::Evaluation, {slot("expr"), slot("unit")});
    if (root && kind == "symbolic") {
        lemma::MathNode symbolic(Kind::Symbolic, {slot("expr", Place::Expression)});
        for (const QJsonValue& k : o.value("keywords").toArray()) {
            symbolic.children.push_back(fromJson(k, Place::KeywordSlot));
        }
        const bool named = std::all_of(symbolic.children.begin() + 1, symbolic.children.end(), [](const auto& k) {
            return k.kind == Kind::Placeholder || k.kind == Kind::Identifier || k.kind == Kind::Keyword;
        });
        if (symbolic.children.size() < 2 || !named) {
            throw BadTree{};
        }
        return symbolic;
    }
    if (place == Place::KeywordSlot && kind == "keyword") {
        lemma::MathNode keyword(Kind::Keyword, {slot("name")});
        for (const QJsonValue& arg : o.value("args").toArray()) {
            keyword.children.push_back(fromJson(arg, Place::Argument));
        }
        const Kind name = keyword.children[0].kind;
        if (keyword.children.size() < 2 || (name != Kind::Identifier && name != Kind::Placeholder)) {
            throw BadTree{};
        }
        return keyword;
    }
    if ((root || place == Place::Expression || place == Place::Argument) && kind == "equation") {
        return lemma::MathNode(Kind::Equation, {slot("lhs"), slot("rhs")});
    }
    throw BadTree{};
}

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
                {"math", toJson(r.math)},
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
    const int version = root.value("version").toInt();
    if (version > kVersion) {
        *error = QObject::tr("This worksheet was saved by a newer version of Lemma.");
        return false;
    }
    if (version < kVersion) {
        *error = QObject::tr("This worksheet was saved by an older pre-release of Lemma, and can't be opened.");
        return false;
    }

    lemma::Worksheet loaded;
    for (const QJsonValue& value : root.value("regions").toArray()) {
        const QJsonObject r = value.toObject();
        const QString type = r.value("type").toString();
        if (type == "math") {
            lemma::MathNode math;
            try {
                math = fromJson(r.value("math"), Place::Root);
            } catch (const BadTree&) {
                *error = QObject::tr("This worksheet is damaged: a math region can't be read.");
                return false;
            }
            loaded.addRegion(r.value("x").toDouble(), r.value("y").toDouble(), std::move(math));
        } else if (type == "text") {
            loaded.addTextRegion(r.value("x").toDouble(), r.value("y").toDouble(), r.value("width").toDouble(),
                                    r.value("text").toString().toStdString());
        } else if (type == "plot") {
            const int id = loaded.addPlotRegion(r.value("x").toDouble(), r.value("y").toDouble(),
                                                   r.value("width").toDouble(), r.value("height").toDouble(),
                                                   r.value("traces").toString().toStdString());
            lemma::PlotAxes& axes = loaded.region(id)->axes;
            axes.x = r.value("xVariable").toString().toStdString();
            axes.xMin = r.value("xMin").toString().toStdString();
            axes.xMax = r.value("xMax").toString().toStdString();
            axes.yMin = r.value("yMin").toString().toStdString();
            axes.yMax = r.value("yMax").toString().toStdString();
        }
    }
    loaded.format = worksheet.format;
    worksheet = std::move(loaded); // only once all of it has loaded
    worksheet.evaluate();
    return true;
}

} // namespace DocumentIO
