#pragma once

#include <QString>

namespace lemma {
class Worksheet;
}

// Reads and writes .lemma files: versioned JSON, so worksheets stay readable and diffable.
namespace DocumentIO {

bool save(const lemma::Worksheet& worksheet, const QString& path, QString* error);
bool load(lemma::Worksheet& worksheet, const QString& path, QString* error);

} // namespace DocumentIO
