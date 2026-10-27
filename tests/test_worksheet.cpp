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

TEST_CASE("A symbolic evaluation shows its expression simplified")
{
    CHECK(lastValue({"2*x + 3*x ->"}) == "5 x");
    CHECK(lastValue({"w*L^4/(8*E*I) -> simplify"}) == "w L^4/(8 E I)");
    CHECK(lastValue({"a + a == b*b ->"}) == "2 a == b^2"); // each side
    CHECK(lastValue({"x := 2", "x*x ->"}) == "4");        // definitions are substituted

    Worksheet ws;
    const int unknown = ws.addRegion(0, 0, "x -> frobnicate");
    const int later = ws.addRegion(0, 20, "x^2 - 1 -> solve, x; solve, x");
    const int zero = ws.addRegion(0, 40, "1/(x - x) ->");
    ws.evaluate();
    const Region& u = *ws.region(unknown);
    CHECK(u.result.error == "Unknown keyword: frobnicate");
    CHECK(u.result.errorNode == u.math.children[1].id);
    const Region& l = *ws.region(later);
    CHECK(l.result.error == "Only one solve to a region");
    CHECK(l.result.errorNode == l.math.children[2].children[0].id);
    const Region& z = *ws.region(zero);
    CHECK(z.result.error == "Division by zero");
    CHECK(z.result.errorNode == z.math.children[0].id);

    CHECK(lastError({"a == b ="}) == "An equation (Ctrl+=) only has a meaning in a symbolic evaluation (→)");
}

TEST_CASE("Symbolic evaluation substitutes the definitions above it (7)")
{
    CHECK(lastValue({"P := 4 kN", "L := 2.5 m", "P*L ->"}) == "10 kN*m");
    CHECK(lastValue({"P := 4 kN", "L := 2.5 m", "P*L -> keep, P"}) == "2.5 P m");
    CHECK(lastValue({"a := b + 1", "b := 2", "a*c ->"}) == "c*(b + 1)"); // a sees what's above it
    CHECK(lastValue({"x := x", "x*x ->"}) == "x^2");                     // Mathcad's idiom
    CHECK(lastValue({"M := 3*a", "M (h/2) ->"}) == "3 a h/2");           // a value times, not a call

    // Functions are inlined; their parameters are never substituted, and their bodies
    // see the definitions above them.
    CHECK(lastValue({"f(x) := x^2 + a", "f(2*b) ->"}) == "4 b^2 + a");
    CHECK(lastValue({"x := 5", "f(x) := x^2", "f(y) ->"}) == "y^2");
    CHECK(lastValue({"a := 1", "f(x) := x + a", "a := 2", "f(y) ->"}) == "y + 1");
    CHECK(lastValue({"f(x) := x^2", "f(t) -> keep, f"}) == "f(t)");
    CHECK(lastError({"f(x) := x^2", "f(1, 2) ->"}) == "f takes 1 argument");
    CHECK(lastError({"f(x) := x^2", "f ->"}) == "f is a function; give it arguments, like f(x)");

    // An error in a definition is reported on the name that used it.
    Worksheet ws;
    ws.addRegion(0, 0, "P := 1 m + 1 kg");
    const int use = ws.addRegion(0, 20, "2*P ->");
    ws.evaluate();
    const Region& u = *ws.region(use);
    CHECK(u.result.error == "In P: Units don't match: m and kg");
    CHECK(u.result.errorNode == u.math.children[0].children[1].id);
}


TEST_CASE("Derivative and integral operators under → (15.2, 15.4)")
{
    // The cantilever's deflection, with its slope defined by the operator: no → and no keep.
    const std::string y = "y(x) := P*x^2*(3*L - x)/(6*E*I)";
    const std::string theta = "θ(x) := diff(y(x), x)";
    CHECK(lastValue({y, theta, "θ(x) -> keep, P, L, E, I"}) == "P x*(2 L - x)/(2 E I)");
    CHECK(lastValue({y, theta, "integrate(θ(x), x, 0, L) -> keep, P, L, E, I"}) == "P L^3/(3 E I)");
    CHECK(lastValue({"P := 4 kN", "L := 2.5 m", "E := 200 GPa", "I := 12500000/3 mm^4", y, theta,
                     "integrate(θ(x), x, 0, L) ->"})
          == "0.025 m");
    // The same as the keywords.
    CHECK(lastValue({y, "diff(y(x), x) -> keep, P"}) == lastValue({y, "y(x) -> keep, P; diff, x"}));

    // Its variable is the main variable, as a keyword's is (section 11).
    CHECK(lastValue({"diff(a*x^2 + b*x, x) ->"}) == "2 a x + b");

    // A derivative's variable then means what it does where the operator stands (15.1).
    CHECK(lastValue({"x := 2", "diff(x^3, x) ->"}) == "12");
    CHECK(lastValue({"f(t) := diff(t^3, t) ->", "f(2) ="}) == "12");
    // Units: the variable has a value inside the operand, so 2 m is a length.
    CHECK(lastValue({"diff(x*2 m, x) ->"}) == "2 m");
    CHECK(lastValue({"integrate(2 N, x, 0, 3 m) ->"}) == "6 N*m");

    // Beyond the engine, a note, not an error.
    const Worksheet ws = sheet({"integrate(exp(x^2), x, 0, 1) ->"});
    CHECK(last(ws).status == RegionResult::Status::Ok);
    CHECK(last(ws).note == "Couldn't integrate this");
    CHECK(last(ws).value == "integrate(e^x^2, x, 0, 1)");
}

TEST_CASE("Derivative and integral operators evaluate numerically (15.3)")
{
    // The cantilever: its slope at the tip, and the tip deflection as the slope's integral.
    const std::initializer_list<std::string> cantilever = {"P := 4 kN", "L := 2.5 m", "E := 200 GPa",
                                                           "I := 12500000/3 mm^4", "y(x) := P*x^2*(3*L - x)/(6*E*I)",
                                                           "θ(x) := diff(y(x), x)"};
    const auto with = [&](const std::string& last) {
        std::vector<std::string> lines(cantilever);
        lines.push_back(last);
        Worksheet ws;
        double y = 0;
        for (const std::string& line : lines) {
            ws.addRegion(0, y, line);
            y += 20;
        }
        ws.evaluate();
        INFO(ws.regions().back().result.error);
        return ws.regions().back().result.value;
    };
    CHECK(with("θ(L) =") == "0.015");
    CHECK(with("integrate(θ(x), x, 0, L) = mm") == "25 mm"); // a limit of 0 takes L's units
}

TEST_CASE("Numeric derivatives are exact, at the variable's value where they stand (15.3)")
{
    CHECK(lastValue({"x := 2", "diff(x^3, x) ="}) == "12");
    CHECK(lastValue({"f(x) := diff(x^3, x, 2)", "f(2) ="}) == "12");
    CHECK(lastValue({"t := 0.5", "diff(sin(3*t) + t^2, t) ="}) == "1.212");
    // Units and definitions mean what they do numerically.
    CHECK(lastValue({"v := 2 m/s", "x(t) := v*t + 9.81 m/s^2*t^2/2", "t := 2 s", "diff(x(t), t) ="}) == "21.62 m/s");
    CHECK(lastError({"diff(x^3, x) ="}) == "x is not defined");
    CHECK(lastError({"x := 1", "diff(max(x, 2*x), x) ="}) == "Couldn't differentiate max");
    CHECK(lastError({"x := 1", "diff(x^2, x, 0) ="}) == "The order is a whole number from 1 to 99");
}

TEST_CASE("Definite integrals are exact where they can be, and otherwise numeric (15.3)")
{
    CHECK(lastValue({"integrate(exp(x^2), x, 0, 1) ="}) == "1.463"); // by quadrature
    CHECK(lastValue({"integrate(sqrt(1 - x^2), x, -1, 1) ="}) == "1.571");
    CHECK(lastValue({"integrate(max(x, 0.5), x, 0, 1) ="}) == "0.625");
    CHECK(lastValue({"integrate(diff(x^3, x), x, 0, 2) ="}) == "8");
    CHECK(lastValue({"integrate(integrate(x*y, x, 0, 1), y, 0, 2) ="}) == "1");
    // The variable is the integral's own.
    CHECK(lastValue({"x := 100", "integrate(x, x, 0, 1) ="}) == "0.5");
    // Units: the integrand's times the variable's.
    CHECK(lastValue({"integrate(2 N, x, 0, 3 m) ="}) == "6 J");
    CHECK(lastValue({"integrate(x, x, 0, 3 m) ="}) == "4.5 m²");
    CHECK(lastError({"integrate(x, x, 1 s, 3 m) ="}) == "Units don't match: the limits are s and m");
    CHECK(lastError({"integrate(1/x, x, 0, 1) ="}) == "This integral doesn't converge");
    CHECK(lastError({"integrate(a*x, x, 0, 1) ="}) == "a is not defined");

    // An indefinite integral needs the engine's antiderivative.
    CHECK(lastValue({"x := 3", "integrate(x^2, x) ="}) == "9");
    CHECK(lastError({"x := 3", "integrate(exp(x^2), x) ="})
          == "Couldn't integrate this; give it limits to integrate numerically");
}

TEST_CASE("A plot of a derivative matches one of the function it was defined as (15.4)")
{
    Worksheet ws;
    ws.addRegion(0, 0, "y(x) := x^3/6 - x");
    ws.addRegion(0, 20, "θ(x) := x^2/2 - 1");
    const int plot = ws.addPlotRegion(0, 40, 288, 192, "diff(y(x), x), θ(x)");
    ws.evaluate();
    const RegionResult& r = ws.region(plot)->result;
    INFO(r.error);
    REQUIRE(r.status == RegionResult::Status::Ok);
    REQUIRE(r.plot.traces.size() == 2);
    const auto& derivative = r.plot.traces[0].segments.at(0);
    const auto& theta = r.plot.traces[1].segments.at(0);
    REQUIRE(derivative.size() == theta.size());
    for (std::size_t i = 0; i < theta.size(); ++i) {
        CHECK(derivative[i].y == Catch::Approx(theta[i].y));
    }
}

TEST_CASE("A parameter named like a unit stays apart from the unit (8)")
{
    // As in the classical mechanics example, with the distance downrange called s.
    const auto trajectory = [](const std::string& x) {
        return sheet({
            "v_0 := 20 m/s",
            "y(t) := v*sin(θ)*t - g*t^2/2",
            "path(" + x + ") := y(t) -> substitute, t == " + x + "/(v*cos(θ))",
            "trajectory(" + x + ", θ) := path(" + x + ") -> substitute, v == v_0",
            "trajectory(10 m, 45 deg) = m",
        });
    };
    const Worksheet named = trajectory("s");
    const Worksheet other = trajectory("X");
    std::string expected = other.regions()[3].result.value;
    for (std::size_t at = expected.find('X'); at != std::string::npos; at = expected.find('X', at)) {
        expected[at] = 's';
    }
    CHECK(named.regions()[3].result.value == expected);
    CHECK(last(named).value == "7.548 m");
    CHECK(last(other).value == "7.548 m");

    // The parameter doesn't make the units in a definition free (P := 4 kN still has
    // units), and a function's own units are units.
    CHECK(lastValue({"v := 2 m/s", "f(s) := s*v ->", "f(3) = m/s"}) == "6 m/s");
    CHECK(lastValue({"f(x) := x + 2 m ->", "f(1 m) = m"}) == "3 m");
    // A unit from a definition above stays a unit in a result, even where a variable
    // of its name is defined in between.
    CHECK(lastValue({"v := 20 m/s", "s := 3", "w := v ->", "w = m/s"}) == "20 m/s");
}
TEST_CASE("A unit name is a unit after a number, where nothing else is free (8)")
{
    Worksheet ws;
    const int free = ws.addRegion(0, 0, "w*L^4/(8*E*I) ->");
    const int quantity = ws.addRegion(0, 20, "2.5 m*3 ->");
    const int derivation = ws.addRegion(0, 40, "1/2 m*v^2 + 3 L - x ->");
    ws.evaluate();
    const Region& f = *ws.region(free);
    CHECK(f.result.value == "w L^4/(8 E I)");
    CHECK(f.result.units.empty()); // L is a length here, not a litre
    const Region& q = *ws.region(quantity);
    CHECK(q.result.value == "7.5 m");
    CHECK(q.result.units.size() == 2); // the m typed, and the m of the result, are upright
    // With free names, unit names are names too: a mass, and a length.
    const Region& d = *ws.region(derivation);
    CHECK(d.result.value == "m v^2/2 + 3 L - x");
    CHECK(d.result.units.empty());
    // Definitions are read the same way: P's value has units, even where P is used with x.
    CHECK(lastValue({"P := 4 kN", "P*x ->"}) == "4 x kN");
    CHECK(lastValue({"f(x) := x + 2 m", "f(1 cm) ->"}) == "201 cm"); // a parameter isn't free

    CHECK(lastValue({"2 m*3 m ->"}) == "6 m^2");
    CHECK(lastValue({"9.81 m/s^2*2 s ->"}) == "19.62 m/s");
    CHECK(lastValue({"g*t^2/2 ->"}) == "g t^2/2");
    CHECK(lastValue({"sin(30 deg) ->"}) == "1/2");
    CHECK(lastValue({"m*2 m -> keep, m"}) == "2 m^2"); // kept, m is a variable
}

TEST_CASE("Terms in different units of one dimension are converted to the first one's (8)")
{
    CHECK(lastValue({"2 m + 3 mm ->"}) == "2.003 m");
    CHECK(lastValue({"3 mm + 2 m ->"}) == "2.003*10^3 mm"); // as numeric results show it
    CHECK(lastValue({"1 ft + 1 in ->"}) == "1.083 ft");
    CHECK(lastValue({"a := 2", "a*1 m + a*1 cm ->"}) == "2.02 m");
    CHECK(lastValue({"d := 1 cm", "x*1 m + x*d -> keep, x"}) == "m x + x cm"); // with x free, this m is a name
    CHECK(lastValue({"1 kN + 1 N ->"}) == "1.001 kN");
}

TEST_CASE("Units are checked as they are numerically (8)")
{
    Worksheet ws;
    const int sum = ws.addRegion(0, 0, "1 m + 1 kg ->");
    ws.evaluate();
    const Region& r = *ws.region(sum);
    CHECK(r.result.error == "Units don't match: m and kg");
    CHECK(r.result.errorNode == r.math.children[0].id);

    CHECK(lastValue({"a := 1 m", "x + a ->"}) == "x + 1 m"); // x's dimension is unknown: allowed
    CHECK(lastError({"2^(1 m) ->"}) == "An exponent can't have units (it has m)");
    CHECK(lastError({"sin(2 m) ->"}) == "sin needs a unitless argument (it has m)");
    CHECK(lastError({"q := 2 m", "q^x ->"}) == "A quantity with units can only be raised to a number");
}

TEST_CASE("keep and real take names (5.3, 7)")
{
    CHECK(lastValue({"sqrt(y^2) ->"}) == "y");
    CHECK(lastValue({"sqrt(y^2) -> real, y"}) == "abs(y)");
    CHECK(lastValue({"y := -3 m", "sqrt(y^2) ->"}) == "3 m");
    CHECK(lastValue({"a := 1", "b := 2", "a + b -> keep, a; simplify"}) == "a + 2");
    CHECK(lastError({"x -> keep"}) == "keep needs the names it applies to, like keep, x");
    CHECK(lastError({"x -> keep, 2"}) == "keep takes names");
}

TEST_CASE("A product's units are tidied, and measurements shown as decimals (8)")
{
    const std::initializer_list<std::string> beam = {"L := 2.5 m", "P := 4 kN", "E := 200 GPa", "b := 50 mm",
                                                     "h := 100 mm", "I := b*h^3/12"};
    const auto with = [&](const std::string& last) {
        std::vector<std::string> lines(beam);
        lines.push_back(last);
        Worksheet ws;
        double y = 0;
        for (const std::string& line : lines) {
            ws.addRegion(0, y, line);
            y += 20;
        }
        ws.evaluate();
        INFO(ws.regions().back().result.error);
        return ws.regions().back().result.value;
    };
    CHECK(with("P*L^3/(3*E*I) ->") == "0.025 m"); // kN·m³/(GPa·mm⁴) is a length: m, the first
    CHECK(with("I ->") == "4.167*10^6 mm^4");     // not 12500000/3 mm⁴
    CHECK(with("P/L ->") == "1.6 kN/m");          // kept as typed
    CHECK(with("P*L^3/(3*E*I) -> keep, P, L, E, I") == "P L^3/(3 E I)");
    CHECK(lastValue({"1 m*1 mm ->"}) == "0.001 m^2");
    CHECK(lastValue({"1 cm/(1 mm) ->"}) == "10"); // no units left
}

TEST_CASE("expand multiplies out (9.2)")
{
    CHECK(lastValue({"(a + b)^3 -> expand"}) == "a^3 + 3 a^2 b + 3 a b^2 + b^3");
    CHECK(lastValue({"(x + 1)*(x - 1) -> expand"}) == "x^2 - 1");
    CHECK(lastValue({"(a + b)/c -> expand"}) == "a/c + b/c");
    CHECK(lastValue({"1/(x + 1)^2 -> expand"}) == "1/(x^2 + 2 x + 1)");
    CHECK(lastValue({"ln(2*x*y^2) -> expand"}) == "ln(x) + 2 ln(y) + ln(2)");
    CHECK(lastValue({"2*(x + 1) -> expand"}) == "2 x + 2");
    CHECK(lastError({"(a + b + c + d + e1)^40 -> expand"}) == "This expands to too many terms");
}

TEST_CASE("substitute replaces names, then simplifies (9.4)")
{
    CHECK(lastValue({"x^2 + y -> substitute, x == 3"}) == "y + 9");
    CHECK(lastValue({"x - y -> substitute, x == y, y == x"}) == "-x + y"); // all at once
    CHECK(lastValue({"a*x -> substitute, x == 2 m"}) == "2 a m");
    CHECK(lastValue({"x^2 -> substitute, x == a + 1; expand"}) == "a^2 + 2 a + 1");
    CHECK(lastError({"x -> substitute"}) == "substitute needs what to replace, like substitute, x == 1");
    CHECK(lastError({"x -> substitute, x"}) == "Write what to substitute as name == value (Ctrl+=)");
}

TEST_CASE("diff differentiates (9.6)")
{
    CHECK(lastValue({"x^3 -> diff, x"}) == "3 x^2");
    CHECK(lastValue({"x^3 -> diff, x, 2"}) == "6 x");
    CHECK(lastValue({"a*x^2 + b*x + c -> diff, x"}) == "2 a x + b");
    CHECK(lastValue({"sin(k*x) -> diff, x"}) == "k cos(k x)");
    CHECK(lastValue({"exp(-k*t) -> diff, t"}) == "-(k e^-(k t))");
    CHECK(lastValue({"ln(x) -> diff, x"}) == "1/x");
    CHECK(lastValue({"sqrt(x) -> diff, x"}) == "1/(2 sqrt(x))");
    CHECK(lastValue({"x*sin(x) -> diff, x"}) == "x cos(x) + sin(x)");
    CHECK(lastValue({"a^x -> diff, x"}) == "a^x ln(a)");
    CHECK(lastValue({"atan(x) -> diff, x"}) == "1/(x^2 + 1)");
    CHECK(lastValue({"y -> diff, x"}) == "0");
    // The beam: deflection to slope, with the names kept.
    CHECK(lastValue({"y(x) := P*x^2*(3*L - x)/(6*E*I)", "y(x) -> diff, x"}) == "P x*(2 L - x)/(2 E I)");

    // An undefined function is beyond diff's scope: a note, not an error.
    const Worksheet ws = sheet({"f(x) + x^2 -> diff, x"});
    CHECK(last(ws).status == RegionResult::Status::Ok);
    CHECK(last(ws).value == "x^2 + f(x)");
    CHECK(last(ws).note == "Couldn't differentiate f, a function with no definition");

    CHECK(lastError({"x -> diff"}) == "diff needs a variable, like diff, x");
    CHECK(lastError({"x -> diff, x, 0"}) == "How many times to differentiate: a whole number, like 2");
}

TEST_CASE("factor takes out common factors and factors polynomials (9.3)")
{
    CHECK(lastValue({"6*a*x + 9*a -> factor"}) == "3 a*(2 x + 3)"); // Copy writes "*" between a name and "("
    CHECK(lastValue({"x^2 - 5*x + 6 -> factor"}) == "(x - 3) (x - 2)");
    CHECK(lastValue({"2*x^2 + x - 1 -> factor"}) == "(x + 1) (2 x - 1)");
    CHECK(lastValue({"x^2 - y^2 -> factor"}) == "(x - y) (x + y)");
    CHECK(lastValue({"x^2 + 2*x*y + y^2 -> factor"}) == "(x + y)^2");
    CHECK(lastValue({"x^3 - 8 -> factor"}) == "(x - 2) (x^2 + 2 x + 4)");
    CHECK(lastValue({"x^4 - y^4 -> factor"}) == "(x - y) (x + y) (x^2 + y^2)");
    CHECK(lastValue({"x^2 - 2 -> factor"}) == "(x - sqrt(2)) (x + sqrt(2))");
    CHECK(lastValue({"sin(t)^2 - 1 -> factor"}) == "(sin(t) - 1) (sin(t) + 1)");
    CHECK(lastValue({"(x^2 - 1)/(x + 2) -> factor"}) == "(x - 1) (x + 1)/(x + 2)");

    // What it can't factor further comes with a note.
    const Worksheet ws = sheet({"x^2 + 1 -> factor"});
    CHECK(last(ws).value == "x^2 + 1");
    CHECK(last(ws).note == "Couldn't factor this further");
    CHECK(sheet({"x^2 - 1 -> factor"}).regions().back().result.note.empty());
}

TEST_CASE("Simplify cancels the polynomial factors a fraction shares (9.1)")
{
    CHECK(lastValue({"(x^2 - 1)/(x - 1) ->"}) == "x + 1");
    CHECK(lastValue({"(x^3 - 1)/(x - 1) ->"}) == "x^2 + x + 1");
    CHECK(lastValue({"(x^2 - 5*x + 6)/(x - 2) ->"}) == "x - 3");
    CHECK(lastValue({"(x^2 - y^2)/(x + y) ->"}) == "x - y");
    CHECK(lastValue({"(a*x^2 - a)/(x + 1) ->"}) == "a*(x - 1)");
    CHECK(lastValue({"(x^2 - 5*x + 6)/(x - 1) ->"}) == "(x^2 - 5 x + 6)/(x - 1)"); // nothing shared
}

TEST_CASE("solve finds every solution (9.5)")
{
    CHECK(lastValue({"M == s*I/c -> solve, s"}) == "s == M c/I");
    CHECK(lastValue({"x^2 - 5*x + 6 -> solve, x"}) == "x == 3, x == 2");
    CHECK(lastValue({"a*x^2 + b*x + c -> solve, x"}) == "x == (-b ± sqrt(b^2 - 4 a c))/(2 a)");
    CHECK(lastValue({"F == k*d^2 -> solve, d"}) == "d == ±(sqrt(F)/sqrt(k))");
    CHECK(lastValue({"y == exp(k*t) -> solve, t"}) == "t == ln(y)/k");
    CHECK(lastValue({"x^2 == 2 -> solve, x"}) == "x == ±sqrt(2)");
    CHECK(lastValue({"1/x == a -> solve, x"}) == "x == 1/a");
    CHECK(lastValue({"ln(x) == 2 -> solve, x"}) == "x == e^2");
    CHECK(lastValue({"2*x + 3 -> solve, x"}) == "x == -(3/2)"); // a bare expression: where it's zero
    CHECK(lastValue({"x := 5", "x^2 == 9 -> solve, x"}) == "x == ±3"); // the variable stays a name
    CHECK(lastValue({"P := 4 kN", "P == k*d -> solve, d"}) == "d == 4 kN/k");
    CHECK(lastValue({"x^2 == 4*a -> solve, x; substitute, a == 4"}) == "x == ±4"); // then on each
    // Sines and cosines of one argument, rewritten so x is in one function.
    CHECK(lastValue({"cos(x)^2 - sin(x)^2 -> solve, x"}) == "x == π/4");
    CHECK(lastValue({"sin(x) - cos(x) -> solve, x"}) == "x == π/4");
    CHECK(lastValue({"sin(x)*cos(x) == 1/4 -> solve, x"}) == "x == π/12");
    CHECK(lastValue({"a*sin(k*t) == b*cos(k*t) -> solve, t"}) == "t == atan(b/a)/k");
    CHECK(lastValue({"2*v^2*sin(θ)*cos(θ)/g -> diff, θ; solve, θ"}) == "θ == π/4"); // the best launch angle

    const auto note = [](std::initializer_list<std::string> lines) {
        const Worksheet ws = sheet(lines);
        REQUIRE(last(ws).status == RegionResult::Status::Ok);
        return last(ws).note;
    };
    CHECK(note({"x^5 + x + 1 -> solve, x"}) == "Couldn't solve this for x");
    CHECK(note({"x^2 + 1 -> solve, x"}) == "There's no real solution for x");
    CHECK(note({"x + 1 == x -> solve, x"}) == "There's no solution for x");
    CHECK(note({"x == x -> solve, x"}) == "Any x is a solution");
    CHECK(note({"sin(x) + cos(x) == 1 -> solve, x"}) == "Couldn't solve this for x");
    CHECK(note({"2*sin(x)*cos(x) == 2 -> solve, x"}) == "There's no real solution for x");
}

TEST_CASE("integrate integrates (9.7)")
{
    CHECK(lastValue({"x^2 -> integrate, x"}) == "x^3/3");
    CHECK(lastValue({"1/x -> integrate, x"}) == "ln(x)");
    CHECK(lastValue({"3*x^2 + 2*x + 1 -> integrate, x"}) == "x^3 + x^2 + x");
    CHECK(lastValue({"sin(k*x + p) -> integrate, x"}) == "-(cos(k x + p)/k)");
    CHECK(lastValue({"exp(-a*t) -> integrate, t"}) == "-(e^-(a t)/a)");
    CHECK(lastValue({"x*sin(k*x) -> integrate, x"}) == "sin(k x)/k^2 - x cos(k x)/k");
    CHECK(lastValue({"x^2*exp(x) -> integrate, x"}) == "e^x*(x^2 - 2 x + 2)");
    CHECK(lastValue({"1/(2*x + 1) -> integrate, x"}) == "ln(2 x + 1)/2"); // positive: no |…|
    CHECK(lastValue({"x/(x + 1)^2 -> integrate, x"}) == "1/(x + 1) + ln(x + 1)");
    CHECK(lastValue({"1/(x - a) -> integrate, x"}) == "ln(abs(x - a))");
    CHECK(lastValue({"x*(x + 1) -> integrate, x"}) == "x^3/3 + x^2/2");
    CHECK(lastValue({"w*x -> integrate, x, 0, L"}) == "w L^2/2");
    CHECK(lastValue({"w := 2 kN/m", "L := 2.5 m", "w*x -> integrate, x, 0, L"}) == "6.25 kN*m");
    CHECK(lastValue({"cos(x) -> integrate, x, 0, pi/2"}) == "1");

    const Worksheet ws = sheet({"exp(x^2) -> integrate, x"});
    CHECK(last(ws).status == RegionResult::Status::Ok);
    CHECK(last(ws).value == "e^x^2");
    CHECK(last(ws).note == "Couldn't integrate this");
    CHECK(lastError({"x -> integrate, x, 0"}) == "integrate needs a variable, and its limits if it has them: integrate, x, 0, L");
}

TEST_CASE("A symbolic definition defines its name as the result (7.1)")
{
    const std::initializer_list<std::string> beam = {
        "P := 4 kN", "L := 2.5 m", "E := 200 GPa", "I := 4166666.667 mm^4",
        "y(x) := P*x^2*(3*L - x)/(6*E*I)",
        "θ(x) := y(x) -> diff, x; keep, P, L, E, I",
    };
    const auto with = [&](const std::string& last) {
        std::vector<std::string> lines(beam);
        lines.push_back(last);
        Worksheet ws;
        double y = 0;
        for (const std::string& line : lines) {
            ws.addRegion(0, y, line);
            y += 20;
        }
        ws.evaluate();
        return ws;
    };
    // Shown after the arrow, and evaluated numerically below: the slope at the tip.
    const Worksheet ws = with("θ(L) =");
    CHECK(ws.regions()[5].result.value == "P x*(2 L - x)/(2 E I)");
    CHECK(ws.regions()[5].result.kind == Statement::Kind::FunctionDefinition);
    CHECK(ws.regions()[6].result.value == "0.015");
    // Used in another →, as if the result had been typed.
    CHECK(with("θ(x) -> keep, P, L, E, I").regions().back().result.value == "P x*(2 L - x)/(2 E I)");
    CHECK(with("θ(a) ->").regions().back().result.value == "0.002 a*(-a + 5 m)/m^2"); // units tidied: m⁻²

    // A variable, evaluated where it's defined: w is known numerically.
    CHECK(lastValue({"w := 2 kN/m", "L := 2.5 m", "M_max := w*L^2/8 -> keep, w", "M_max = kN*m"}) == "1.562 kN*m");
    // Parameters are never substituted.
    CHECK(lastValue({"x := 5", "f(x) := x^3 -> diff, x", "f(2) ="}) == "12");
    CHECK(lastValue({"x := 5", "f(x) := x^3 -> diff, x", "f(t) ->"}) == "3 t^2");
    // A lone solution: the name is its right-hand side, shown as solved.
    const Worksheet peak = sheet({"t_peak := v*sin(θ) - g*t == 0 -> solve, t", "t_peak ->"});
    CHECK(peak.regions()[0].result.value == "t == v sin(θ)/g");
    CHECK(last(peak).value == "v sin(θ)/g");
    CHECK(lastValue({"v := 20 m/s", "g := 9.8 m/s^2", "t_peak := v - g*t == 0 -> solve, t", "t_peak ="})
          == "2.041 s");
    CHECK(lastValue({"r(a) := a*x - 6 == 0 -> solve, x", "r(3) ="}) == "2");
    // Full precision, not the 3 places shown.
    CHECK(lastValue({"k := 1/3 -> simplify", "k*3 ="}) == "1");

    // An operation beyond its scope: shown with its note, and no value to use.
    const Worksheet open = sheet({"F(x) := exp(x^2) -> integrate, x", "F(1) ="});
    CHECK(open.regions()[0].result.status == RegionResult::Status::Ok);
    CHECK(open.regions()[0].result.note == "Couldn't integrate this");
    CHECK(last(open).error == "F has no closed form");
    CHECK(lastError({"s := x^2 - 1 -> solve, x"}) == "solve found 2 solutions; a definition needs one");
    CHECK(lastError({"s := x^2 == 1 -> simplify"}) == "A definition needs one result, not an equation");
    CHECK(lastError({"s := x == 1"}) == "An equation (Ctrl+=) only has a meaning in a symbolic evaluation (→)");
    CHECK(lastError({"q := 1/(a - a) -> simplify", "q ="}) == "q is not defined");
}
