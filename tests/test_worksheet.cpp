#include "lemma/worksheet.h"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <string>

using namespace lemma;

namespace {

// Builds a worksheet with one region per line, top to bottom, and evaluates it.
Worksheet sheet(std::initializer_list<std::string> lines)
{
    Worksheet ws;
    double y = 0;
    for (const std::string& line : lines) {
        ws.addRegion(0, y, line);
        y += 20;
    }
    ws.evaluate();
    return ws;
}

const RegionResult& last(const Worksheet& ws)
{
    return ws.regions().back().result;
}

std::string lastValue(std::initializer_list<std::string> lines)
{
    const Worksheet ws = sheet(lines);
    INFO(last(ws).error);
    REQUIRE(last(ws).status == RegionResult::Status::Ok);
    return last(ws).value;
}

std::string lastError(std::initializer_list<std::string> lines)
{
    const Worksheet ws = sheet(lines);
    REQUIRE(last(ws).status == RegionResult::Status::Error);
    return last(ws).error;
}

} // namespace

TEST_CASE("Plain arithmetic")
{
    CHECK(lastValue({"2 + 3 * 4 ="}) == "14");
    CHECK(lastValue({"2^10 ="}) == "1.024×10³");
    CHECK(lastValue({"sqrt(2) ="}) == "1.414");
}

TEST_CASE("Definitions carry units through calculations")
{
    const Worksheet ws = sheet({"L := 2 m", "F := 10 N", "L·F ="});
    CHECK(last(ws).value == "20 J");
    CHECK(last(ws).display == "L·F = 20 J");
}

TEST_CASE("Definitions display with one space around :=")
{
    CHECK(last(sheet({"w:= 2 kN/m"})).display == "w := 2 kN/m");
    CHECK(last(sheet({"w:=2 kN/m"})).display == "w := 2 kN/m");
    CHECK(last(sheet({"w  :=   2 kN/m"})).display == "w := 2 kN/m");
    CHECK(last(sheet({"w := (1 + 2) kN/m"})).display == "w := (1 + 2) kN/m");
    CHECK(last(sheet({"f(x, y):=x y"})).display == "f(x, y) := x y");
}

TEST_CASE("Results can be shown in a chosen unit")
{
    CHECK(lastValue({"10 ft = m"}) == "3.048 m");
    CHECK(lastValue({"L := 2 m", "F := 10 N", "L F = kJ"}) == "0.02 kJ");
    CHECK(lastValue({"v := 60 mi/hr", "v = ft/s"}) == "88 ft/s");
    CHECK(lastValue({"P := 1 atm", "P = psi"}) == "14.696 psi");
}

TEST_CASE("Display units are looked up as units, even if a variable shadows them")
{
    CHECK(lastValue({"m := 5 kg", "m = gm"}) == "5×10³ gm");
    CHECK(lastValue({"m := 5 kg", "x := 2 m", "x ="}) == "10 kg");
}

TEST_CASE("Derived dimensions without a named unit use SI base units")
{
    CHECK(lastValue({"a := 9.81 m/s^2", "a ="}) == "9.81 m/s²");
    CHECK(lastValue({"A := 3 m · 4 m", "A ="}) == "12 m²");
    CHECK(lastValue({"sqrt(16 m^2) ="}) == "4 m");
    CHECK(lastValue({"1 / (2 s) ="}) == "0.5 1/s");
}

TEST_CASE("Mismatched units are errors")
{
    CHECK(lastError({"1 m + 1 kg ="}) == "Units don't match: m and kg");
    CHECK(lastError({"1 m = kg"}) == "Units don't match: the result is m, but kg is kg");
    CHECK(lastError({"2^(1 m) ="}) == "An exponent can't have units (it has m)");
    CHECK(lastError({"sin(1 m) ="}) == "sin needs a unitless argument (it has m)");
}

TEST_CASE("Unit-mismatch errors name dimensions with named units when one fits")
{
    CHECK(lastError({"1 N + 1 kg ="}) == "Units don't match: N and kg");
    CHECK(lastError({"1 J = W"}) == "Units don't match: the result is J, but W is W");
    CHECK(lastError({"1 Pa + 1 m/s^2 ="}) == "Units don't match: Pa and m/s²");
    CHECK(lastError({"1 m/s^2 = N"}) == "Units don't match: the result is m/s², but N is N");
    CHECK(lastError({"2^(1 N) ="}) == "An exponent can't have units (it has N)");
    CHECK(lastError({"1 + 1 m ="}) == "Units don't match: unitless and m");
}

TEST_CASE("Angles and constants")
{
    CHECK(lastValue({"sin(30 deg) ="}) == "0.5");
    CHECK(lastValue({"π ="}) == "3.142");
    CHECK(lastValue({"m := 2 kg", "m g ="}) == "19.613 N");
}

TEST_CASE("User functions")
{
    CHECK(lastValue({"f(x) := x^2", "f(3 m) ="}) == "9 m²");
    CHECK(lastValue({"area(w, h) := w h", "area(2 ft, 3 ft) = ft^2"}) == "6 ft^2");
    CHECK(lastError({"f(x) := x^2", "f(1, 2) ="}) == "f takes 1 argument");
    CHECK(lastError({"f(x) := x^2", "f ="}) == "f is a function; give it arguments, like f(x)");
}

TEST_CASE("Parentheses after a value multiply")
{
    CHECK(lastValue({"M := 10 N m", "h := 0.2 m", "M (h/2) ="}) == "1 kg·m³/s²");
    CHECK(lastValue({"2 kN(3 m) = kN m"}) == "6 kN m");
    CHECK(lastError({"M := 10", "M(1, 2) ="}) == "M is not a function");
    CHECK(lastError({"q(2) ="}) == "q is not defined");
}

TEST_CASE("Functions see definitions above them, not above the call")
{
    CHECK(lastValue({"k := 2", "f(x) := k x", "k := 100", "f(3) ="}) == "6");
    CHECK(lastError({"f(x) := k x", "k := 2", "f(3) ="}) == "In f: k is not defined");
}

TEST_CASE("Evaluation order is top to bottom, then left to right")
{
    Worksheet ws;
    const int use = ws.addRegion(0, 0, "x =");
    ws.addRegion(0, 40, "x := 5");
    ws.evaluate();
    CHECK(ws.region(use)->result.error == "x is not defined");

    ws.region(use)->y = 80;
    ws.evaluate();
    CHECK(ws.region(use)->result.value == "5");

    Worksheet sideBySide;
    sideBySide.addRegion(0, 0, "y := 3");
    const int right = sideBySide.addRegion(200, 0, "y + 1 =");
    sideBySide.evaluate();
    CHECK(sideBySide.region(right)->result.value == "4");
}

TEST_CASE("A restored region keeps its id and its place in the order")
{
    Worksheet ws;
    const int a = ws.addRegion(0, 0, "x := 1");
    const int b = ws.addRegion(0, 0, "x := 2"); // same spot: the later one wins ties
    ws.addRegion(0, 40, "x =");
    const Region removed = *ws.region(a);
    ws.removeRegion(a);
    ws.restoreRegion(removed);

    REQUIRE(ws.regions().size() == 3);
    CHECK(ws.regions()[0].id == a);
    CHECK(ws.regions()[1].id == b);
    ws.evaluate();
    CHECK(ws.regions()[2].result.value == "2");

    // New regions never reuse a restored id.
    CHECK(ws.addRegion(0, 80) > ws.regions()[2].id);
}

TEST_CASE("Errors stop a definition, so later uses report it as undefined")
{
    const Worksheet ws = sheet({"a := 1 m + 1 s", "a ="});
    CHECK(ws.regions()[0].result.status == RegionResult::Status::Error);
    CHECK(last(ws).error == "a is not defined");
}

TEST_CASE("Division by zero")
{
    CHECK(lastError({"1 / (2 - 2) ="}) == "Division by zero");
}
