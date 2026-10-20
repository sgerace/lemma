#include "lemma/worksheet.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <string>
#include <vector>

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
}

TEST_CASE("Results give their number and unit separately, for typesetting")
{
    const Worksheet ws = sheet({"L := 2 m", "F := 10 N", "L·F ="});
    CHECK(last(ws).number == "20");
    REQUIRE(last(ws).unit.has_value());
    CHECK(toLinear(*last(ws).unit) == "J");

    const Worksheet accel = sheet({"a := 9.81 m/s^2", "a ="});
    const RegionResult& base = last(accel);
    CHECK(base.number == "9.81");
    CHECK(toLinear(*base.unit) == "m/s^2");
    CHECK(toLinear(*last(sheet({"x := 2 m^(1/2) kg", "x ="})).unit) == "kg*m^(1/2)");

    // A chosen unit is the region's own unit slot, so the result has none of its own.
    const Worksheet feet = sheet({"10 ft = m"});
    const RegionResult& chosen = last(feet);
    CHECK(chosen.number == "3.048");
    CHECK(!chosen.unit.has_value());
    CHECK(!last(sheet({"2 + 3 ="})).unit.has_value());
}

TEST_CASE("Identifiers that meant units are reported, to show them upright")
{
    Worksheet ws;
    const int id = ws.addRegion(0, 0, "m := 5 kg");
    ws.addRegion(0, 20, "x := 2 m");
    ws.evaluate();
    const MathNode& def = ws.region(id)->math;
    const int kg = def.children[1].children[1].id;
    CHECK(ws.region(id)->result.units == std::vector<int>{kg});
    CHECK(ws.regions()[1].result.units.empty()); // m is the variable here, not metres
}

TEST_CASE("Errors in math regions name the node they are about")
{
    Worksheet ws;
    const int mismatch = ws.addRegion(0, 0, "1 m + 1 kg =");
    const int undefined = ws.addRegion(0, 20, "q + 1 =");
    MathNode definition(MathNode::Kind::Definition, {MathNode::identifier("x"), MathNode()});
    const int hole = definition.children[1].id;
    const int incomplete = ws.addRegion(0, 40, std::move(definition));
    ws.evaluate();

    const Region& m = *ws.region(mismatch);
    CHECK(m.result.errorNode == m.math.children[0].id); // the sum
    const Region& u = *ws.region(undefined);
    CHECK(u.result.error == "q is not defined");
    CHECK(u.result.errorNode == u.math.children[0].children[0].id);
    CHECK(ws.region(incomplete)->result.error == "Fill in the placeholder");
    CHECK(ws.region(incomplete)->result.errorNode == hole);
}

TEST_CASE("An empty math region is just empty")
{
    Worksheet ws;
    const int id = ws.addRegion(0, 0);
    ws.evaluate();
    CHECK(ws.region(id)->result.status == RegionResult::Status::Empty);
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

TEST_CASE("Text regions take no part in evaluation")
{
    Worksheet ws;
    ws.addRegion(0, 0, "x := 2");
    const int note = ws.addTextRegion(0, 20, 300, "x := 5\nso x = 5, surely");
    ws.addRegion(0, 40, "x =");
    ws.evaluate();

    CHECK(ws.region(note)->result.status == RegionResult::Status::Empty);
    CHECK(ws.regions().back().result.value == "2");
}

TEST_CASE("Plots sample a function across the x range")
{
    Worksheet ws;
    ws.addRegion(0, 0, "f(x) := 2 x + 3");
    const int plot = ws.addPlotRegion(0, 20, 288, 192, "f");
    ws.evaluate();

    const RegionResult& r = ws.region(plot)->result;
    INFO(r.error);
    REQUIRE(r.status == RegionResult::Status::Ok);
    REQUIRE(r.plot.traces.size() == 1);
    const auto& points = r.plot.traces[0].segments.at(0);
    REQUIRE(points.size() == kPlotSamples);
    CHECK(points.front().x == -10.0);
    CHECK(points.front().y == -17.0);
    CHECK(points.back().x == 10.0);
    CHECK(points.back().y == 23.0);
    // The y range is fitted to round numbers around the data.
    CHECK(r.plot.yMin == -20.0);
    CHECK(r.plot.yMax == 30.0);
}

TEST_CASE("Plot traces can be expressions, builtins, or several at once")
{
    Worksheet ws;
    ws.addRegion(0, 0, "a := 2");
    const int plot = ws.addPlotRegion(0, 20, 288, 192, "sin, a x^2, max(x, 1)");
    ws.region(plot)->axes.xMin = "0";
    ws.region(plot)->axes.xMax = "3";
    ws.evaluate();

    const RegionResult& r = ws.region(plot)->result;
    INFO(r.error);
    REQUIRE(r.status == RegionResult::Status::Ok);
    REQUIRE(r.plot.traces.size() == 3);
    CHECK(r.plot.traces[1].segments[0].back().y == 18.0);
    CHECK(r.plot.traces[2].segments[0].front().y == 1.0);
    CHECK(r.plot.traces[2].begin == 12); // "max(x, 1)", for coloring the source
    CHECK(r.plot.traces[2].end == 21);
}

TEST_CASE("Plots break the curve where it can't be evaluated")
{
    Worksheet ws;
    const int plot = ws.addPlotRegion(0, 0, 288, 192, "1/x");
    ws.region(plot)->axes.yMin = "-5";
    ws.region(plot)->axes.yMax = "5";
    ws.evaluate();

    const RegionResult& r = ws.region(plot)->result;
    REQUIRE(r.status == RegionResult::Status::Ok);
    CHECK(r.plot.traces[0].segments.size() == 2); // either side of x = 0
    CHECK(r.plot.yMin == -5.0);
    CHECK(r.plot.yMax == 5.0);
}

TEST_CASE("Plots carry units on both axes")
{
    Worksheet ws;
    ws.addRegion(0, 0, "k := 3 N/m");
    ws.addRegion(0, 20, "F(d) := k d");
    const int plot = ws.addPlotRegion(0, 40, 288, 192, "F");
    ws.region(plot)->axes.xMin = "0 m";
    ws.region(plot)->axes.xMax = "200 mm";
    ws.evaluate();

    const RegionResult& r = ws.region(plot)->result;
    INFO(r.error);
    REQUIRE(r.status == RegionResult::Status::Ok);
    CHECK(describeDimension(r.plot.xDim) == "m");
    CHECK(describeDimension(r.plot.yDim) == "N");
    CHECK(r.plot.xMax == 0.2);

    ws.region(plot)->source = "F, x";
    ws.evaluate();
    CHECK(ws.region(plot)->result.error == "Units don't match: this trace is m, but the one before it is N");

    ws.region(plot)->source = "F";
    ws.region(plot)->axes.yMax = "5 kg";
    ws.evaluate();
    CHECK(ws.region(plot)->result.error == "y max: units don't match: the axis is N, but this is kg");
}

TEST_CASE("Plot errors point at the trace or name the field")
{
    const auto error = [](const std::string& traces, PlotAxes axes = {}) {
        Worksheet ws;
        const int plot = ws.addPlotRegion(0, 0, 288, 192, traces);
        ws.region(plot)->axes = std::move(axes);
        ws.evaluate();
        return ws.region(plot)->result;
    };
    const RegionResult undefined = error("x, q");
    CHECK(undefined.error == "q is not defined");
    CHECK(undefined.errorBegin == 3);
    CHECK(undefined.errorEnd == 4);

    CHECK(error("x", {.x = "2"}).error == "x-axis variable: type a name, like x");
    CHECK(error("x", {.xMin = "5", .xMax = "1"}).error == "x max must be greater than x min");
    CHECK(error("x", {.xMin = "0 m", .xMax = "1 s"}).error == "x max: units don't match: the axis is m, but this is s");
    CHECK(error("x", {.xMax = "q"}).error == "x max: q is not defined");

    // An empty plot isn't an error; it just has nothing to draw yet.
    CHECK(error("").status == RegionResult::Status::Ok);
    CHECK(error("x,").status == RegionResult::Status::Ok);
}

TEST_CASE("Nice steps for ticks")
{
    CHECK(niceStep(10) == 2.0);
    CHECK(niceStep(40) == 10.0);
    CHECK(niceStep(0.3) == Catch::Approx(0.05));
    CHECK(niceStep(0) == 1.0);
}
