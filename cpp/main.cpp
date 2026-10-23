// lmmolecule: OpenMM 8 (Core API, Reference and CPU platforms, Lepton, PocketFFT, L-BFGS)
// behind an lmscript `_dsl_` schema. Scripting only: the executable has no user-facing CLI
// besides --dsl and the standard schema/version/notices queries.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "OpenMM.h"
#include "SimTKOpenMMRealType.h"
#include "SimTKOpenMMUtilities.h"
#include "openmm/internal/NonbondedForceImpl.h"
#include "lepton/ExpressionTreeNode.h"
#include "lepton/Operation.h"
#include "lepton/ParsedExpression.h"
#include "lepton/Parser.h"

#include "dslvm.hpp"
#include "memory_budget.hpp"
#include "notices_embedded.hpp"
#include "schema_embedded.hpp"

extern "C" void registerCpuPlatform();

namespace {
using dslvm::Args;
using dslvm::Value;

const double kPi = 3.14159265358979323846;

std::string fmt(double x) {
  if (std::fabs(x) == 0.0) x = 0.0;
  char b[48];
  std::snprintf(b, sizeof b, "%.10g", x);
  return b;
}

std::string lower_str(std::string s) {
  for (char &c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

std::string trim_str(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) a++;
  while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

std::map<std::string, double> parse_params(const std::string &spec, const char *who) {
  std::map<std::string, double> out;
  std::string cur;
  auto flush = [&] {
    std::string tok = trim_str(cur);
    cur.clear();
    if (tok.empty()) return;
    size_t eq = tok.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 >= tok.size())
      throw dslvm::Error(std::string(who) + ": bad parameter '" + tok + "' (expected name=value)");
    std::string name = trim_str(tok.substr(0, eq));
    std::string val_s = trim_str(tok.substr(eq + 1));
    for (size_t i = 0; i < name.size(); i++) {
      char c = name[i];
      bool ok = (i == 0) ? (std::isalpha((unsigned char)c) || c == '_')
                         : (std::isalnum((unsigned char)c) || c == '_');
      if (!ok) throw dslvm::Error(std::string(who) + ": bad parameter name '" + name + "'");
    }
    size_t used = 0;
    double val = 0.0;
    try {
      val = std::stod(val_s, &used);
    } catch (...) {
      used = 0;
    }
    if (used != val_s.size() || !std::isfinite(val))
      throw dslvm::Error(std::string(who) + ": bad parameter value '" + val_s + "'");
    static const std::set<std::string> reserved = {"x", "y", "z", "r", "pi", "e"};
    if (reserved.count(name))
      throw dslvm::Error(std::string(who) + ": parameter name '" + name + "' is reserved");
    if (out.count(name)) throw dslvm::Error(std::string(who) + ": parameter '" + name + "' given twice");
    out[name] = val;
  };
  for (char c : spec) {
    if (c == ',' || c == ';') flush();
    else cur += c;
  }
  flush();
  return out;
}

// Collects the variable names of a parsed Lepton expression and rejects constant sub-expressions that
// are not finite. Lepton's optimize() (run by createCompiledExpression and by every OpenMM custom force
// kernel) compares folded constants with ==, so a constant NaN such as sqrt(-1) never converges and
// hangs the process.
bool lepton_scan(const Lepton::ExpressionTreeNode &n, std::set<std::string> &vars, const std::string &expr,
                 const char *who) {
  if (n.getOperation().getId() == Lepton::Operation::VARIABLE) {
    vars.insert(n.getOperation().getName());
    return true;
  }
  bool has_var = false;
  for (const auto &c : n.getChildren()) has_var = lepton_scan(c, vars, expr, who) || has_var;
  if (!has_var && !std::isfinite(Lepton::ParsedExpression(n).evaluate()))
    throw dslvm::Error(std::string(who) + ": expression \"" + expr + "\": constant sub-expression is not finite");
  return has_var;
}

Lepton::ParsedExpression parse_lepton(const std::string &expr, std::set<std::string> &vars, const char *who) {
  if (trim_str(expr).empty()) throw dslvm::Error(std::string(who) + ": expression is empty");
  Lepton::ParsedExpression parsed;
  try {
    parsed = Lepton::Parser::parse(expr);
  } catch (const std::exception &e) {
    throw dslvm::Error(std::string(who) + ": expression \"" + expr + "\": " + e.what());
  }
  lepton_scan(parsed.getRootNode(), vars, expr, who);
  return parsed;
}

void validate_lepton(const std::string &expr, const std::set<std::string> &allowed, const char *who) {
  std::set<std::string> vars;
  parse_lepton(expr, vars, who);
  for (const auto &v : vars) {
    if (allowed.find(v) == allowed.end() && v != "pi" && v != "e")
      throw dslvm::Error(std::string(who) + ": expression \"" + expr + "\": unknown name '" + v + "'");
  }
}

struct ParticleSpec {
  double mass = 1.0;
  OpenMM::Vec3 pos{0.0, 0.0, 0.0};
  OpenMM::Vec3 vel{0.0, 0.0, 0.0};
  double charge = 0.0;
  bool has_lj = false;
  double lj_eps = 0.0;
  double lj_sigma = 0.1;
  std::string symbol = "Ar";
};

struct BondSpec {
  int i, j;
  double r0, k;
};

struct AngleSpec {
  int i, j, k;
  double th0, kth;
};

struct TorsionSpec {
  int i, j, k, l;
  int periodicity;
  double phase, k_torsion;
};

struct CustomExternalSpec {
  std::string expr;
  std::map<std::string, double> params;
};

struct CustomBondSpec {
  int i, j;
  std::string expr;
};

struct CustomNonbondedSpec {
  std::string expr;
  std::map<std::string, double> params;
  double cutoff;
};

struct ConstraintSpec {
  int i, j;
  double dist;
};

// Rigid three-site water models (OpenMM/GROMACS parameter sets; nm, kJ/mol, e, amu).
struct WaterModel {
  const char *name;
  double r_oh, theta_deg, q_o, q_h, sigma_o, eps_o;
};

const WaterModel *water_model(const std::string &name, const char *who) {
  static const WaterModel models[] = {
      {"tip3p", 0.09572, 104.52, -0.834, 0.417, 0.315061, 0.636386},
      {"spce", 0.1, 109.47, -0.8476, 0.4238, 0.316557, 0.650194},
  };
  std::string m = lower_str(trim_str(name));
  if (m == "spc/e") m = "spce";
  for (const auto &w : models) {
    if (m == w.name) return &w;
  }
  throw dslvm::Error(std::string(who) + ": unknown water model '" + name + "' (expected 'tip3p' or 'spce')");
}

const double MASS_O = 15.99943;
const double MASS_H = 1.007947;

struct SystemSpec {
  bool has_box = false;
  double box_a = 0.0, box_b = 0.0, box_c = 0.0;
  std::vector<ParticleSpec> particles;
  std::vector<BondSpec> bonds;
  std::vector<AngleSpec> angles;
  std::vector<TorsionSpec> torsions;
  std::vector<ConstraintSpec> constraints;
  bool has_lj = false;
  double default_lj_eps = 0.0;
  double default_lj_sigma = 0.1;
  double lj_cutoff = 0.0;
  bool has_charges = false;
  bool has_coulomb = false;
  std::string coulomb_method = "none";
  double coulomb_cutoff = 0.0;
  double rf_dielectric = 78.3;
  std::set<std::pair<int, int>> exclusions;
  std::vector<CustomExternalSpec> custom_externals;
  std::vector<CustomBondSpec> custom_bonds;
  std::vector<CustomNonbondedSpec> custom_nonbondeds;
  bool has_barostat = false;
  double baro_pressure = 1.0;
  double baro_temp = 300.0;
  int baro_freq = 25;
  int baro_seed = 1;
  bool has_andersen = false;
  double andersen_temp = 300.0;
  double andersen_rate = 1.0;
  int andersen_seed = 1;

  // One rigid water: oxygen at (ox, oy, oz), hydrogens along the rotated molecular frame
  // (rot is a 3x3 rotation matrix stored row-major; identity puts the molecule in the xy-plane,
  // bisector along +x).
  void add_water(const WaterModel &w, OpenMM::Vec3 o, const double rot[9]) {
    double half = 0.5 * w.theta_deg * kPi / 180.0;
    double hx = w.r_oh * std::cos(half), hy = w.r_oh * std::sin(half);
    OpenMM::Vec3 local[2] = {OpenMM::Vec3(hx, hy, 0.0), OpenMM::Vec3(hx, -hy, 0.0)};
    int base = (int)particles.size();
    ParticleSpec po;
    po.mass = MASS_O;
    po.pos = o;
    po.charge = w.q_o;
    po.has_lj = true;
    po.lj_eps = w.eps_o;
    po.lj_sigma = w.sigma_o;
    po.symbol = "O";
    particles.push_back(po);
    for (int h = 0; h < 2; h++) {
      const OpenMM::Vec3 &l = local[h];
      ParticleSpec ph;
      ph.mass = MASS_H;
      ph.pos = OpenMM::Vec3(o[0] + rot[0] * l[0] + rot[1] * l[1] + rot[2] * l[2],
                            o[1] + rot[3] * l[0] + rot[4] * l[1] + rot[5] * l[2],
                            o[2] + rot[6] * l[0] + rot[7] * l[1] + rot[8] * l[2]);
      ph.charge = w.q_h;
      ph.has_lj = true;
      ph.lj_eps = 0.0;
      ph.lj_sigma = 0.1;
      ph.symbol = "H";
      particles.push_back(ph);
    }
    has_charges = true;
    has_lj = true;
    double d_hh = 2.0 * hy;
    constraints.push_back(ConstraintSpec{base, base + 1, w.r_oh});
    constraints.push_back(ConstraintSpec{base, base + 2, w.r_oh});
    constraints.push_back(ConstraintSpec{base + 1, base + 2, d_hh});
    exclusions.insert({base, base + 1});
    exclusions.insert({base, base + 2});
    exclusions.insert({base + 1, base + 2});
  }
};

struct SimObj {
  SystemSpec spec;
  std::unique_ptr<OpenMM::System> sys;
  std::unique_ptr<OpenMM::Integrator> integ;
  std::unique_ptr<OpenMM::Context> ctx;
  int mobile_particles = 0;
  int dof = 0;
  int64_t total_steps = 0;
  // PME work memory OpenMM mallocs (outside operator new) at every force evaluation of this Sim.
  std::size_t charged = 0;
  ~SimObj() { membudget::release(charged); }
  std::vector<OpenMM::Vec3> mark_pos;

  std::string report_path;
  int64_t report_every = 0;
  bool report_started = false;
  std::ofstream report_stream;

  std::string xyz_path;
  int64_t xyz_every = 0;
  bool xyz_started = false;
  std::ofstream xyz_stream;

  // Equipartition over the unconstrained degrees of freedom: 3 per mobile particle, minus one per
  // distance constraint (a rigid three-site water keeps 6 of its 9).
  double compute_temp(double ke) const {
    if (dof <= 0) return 0.0;
    return (2.0 * ke) / ((double)dof * BOLTZ);
  }

  void check_box(const char *who) const {
    if (!spec.has_box) return;
    OpenMM::Vec3 a, b, c;
    ctx->getState(0).getPeriodicBoxVectors(a, b, c);
    double v = a[0] * b[1] * c[2];
    if (!std::isfinite(v) || !(v > 0.0) || !(a[0] > 0.0) || !(b[1] > 0.0) || !(c[2] > 0.0))
      throw dslvm::Error(std::string(who) + ": periodic box is no longer valid (volume " + fmt(v) + " nm^3) at step " +
                         std::to_string(total_steps) + " (barostat collapse or blow-up)");
  }

  OpenMM::State energy_state(const char *who) const {
    OpenMM::State st = ctx->getState(OpenMM::State::Energy);
    if (!std::isfinite(st.getKineticEnergy()) || !std::isfinite(st.getPotentialEnergy()))
      throw dslvm::Error(std::string(who) + ": energy is not finite (ke " + fmt(st.getKineticEnergy()) + ", pe " +
                         fmt(st.getPotentialEnergy()) + ") at step " + std::to_string(total_steps));
    return st;
  }

  double force(int64_t i, int d, const char *who) const {
    double f = ctx->getState(OpenMM::State::Forces).getForces()[i][d];
    if (!std::isfinite(f))
      throw dslvm::Error(std::string(who) + ": force on particle " + std::to_string(i) + " is not finite");
    return f;
  }

  void write_report_row() {
    if (!report_stream.is_open()) return;
    OpenMM::State st = energy_state("Sim.report");
    double ke = st.getKineticEnergy();
    double pe = st.getPotentialEnergy();
    double te = ke + pe;
    double temp = compute_temp(ke);
    report_stream << total_steps << " " << fmt(st.getTime()) << " " << fmt(ke) << " " << fmt(pe)
                  << " " << fmt(te) << " " << fmt(temp) << "\n";
    report_stream.flush();
    if (!report_stream) throw dslvm::Error("Sim.report: failed writing to " + report_path);
  }

  // OpenMM's Reference platform integrates NaN/inf without complaint (the CPU platform only notices
  // in its neighbour list), so check the state after every integration chunk and minimization.
  void check_finite(const char *who) const {
    OpenMM::State st = ctx->getState(OpenMM::State::Positions | OpenMM::State::Velocities);
    const auto &pos = st.getPositions();
    const auto &vel = st.getVelocities();
    for (size_t i = 0; i < pos.size(); i++) {
      for (int d = 0; d < 3; d++) {
        if (!std::isfinite(pos[i][d]) || !std::isfinite(vel[i][d]))
          throw dslvm::Error(std::string(who) + ": simulation diverged: non-finite " +
                             (std::isfinite(pos[i][d]) ? "velocity" : "position") + " of particle " +
                             std::to_string(i) + " at step " + std::to_string(total_steps) +
                             " (reduce the timestep or check the forces)");
      }
    }
    check_box(who);
  }

  // Integrates n steps, honouring the CSV/XYZ reporters at their intervals.
  void run_steps(int64_t n) {
    if (!report_path.empty() && !report_started) {
      report_stream.open(report_path, std::ios::out | std::ios::trunc);
      if (!report_stream) throw dslvm::Error("Sim.report: cannot open " + report_path);
      report_stream << "# step time ke pe energy temperature\n";
      report_started = true;
      write_report_row();
    }
    if (!xyz_path.empty() && !xyz_started) {
      xyz_stream.open(xyz_path, std::ios::out | std::ios::trunc);
      if (!xyz_stream) throw dslvm::Error("Sim.xyz: cannot open " + xyz_path);
      xyz_started = true;
      write_xyz_frame(xyz_stream, xyz_path);
    }
    int64_t rem = n;
    while (rem > 0) {
      int64_t chunk = rem;
      if (report_every > 0) chunk = std::min(chunk, report_every - (total_steps % report_every));
      if (xyz_every > 0) chunk = std::min(chunk, xyz_every - (total_steps % xyz_every));
      integ->step((int)chunk);
      total_steps += chunk;
      rem -= chunk;
      check_finite("Sim.step");
      if (report_every > 0 && (total_steps % report_every == 0)) write_report_row();
      if (xyz_every > 0 && (total_steps % xyz_every == 0)) write_xyz_frame(xyz_stream, xyz_path);
    }
  }

  void write_pdb(std::ostream &os, const std::string &path_for_err) const {
    OpenMM::State st = ctx->getState(OpenMM::State::Positions);
    const auto &pos = st.getPositions();
    char line[128];
    if (spec.has_box) {
      OpenMM::Vec3 a, b, c;
      st.getPeriodicBoxVectors(a, b, c);
      std::snprintf(line, sizeof line, "CRYST1%9.3f%9.3f%9.3f  90.00  90.00  90.00 P 1           1\n", a[0] * 10.0,
                    b[1] * 10.0, c[2] * 10.0);
      os << line;
    }
    os << "REMARK   1 written by lmmolecule " LMMOLECULE_VERSION " at step " << total_steps << "\n";
    for (size_t i = 0; i < pos.size(); i++) {
      std::string sym = (i < spec.particles.size() && !spec.particles[i].symbol.empty()) ? spec.particles[i].symbol : "Ar";
      if (sym.size() > 2) sym = sym.substr(0, 2);
      std::string name = sym.size() == 1 ? " " + sym : sym;
      std::snprintf(line, sizeof line, "HETATM%5d %-4s MOL A   1    %8.3f%8.3f%8.3f  1.00  0.00          %2s\n",
                    (int)((i + 1) % 100000), name.c_str(), pos[i][0] * 10.0, pos[i][1] * 10.0, pos[i][2] * 10.0,
                    sym.c_str());
      os << line;
    }
    os << "END\n";
    os.flush();
    if (!os) throw dslvm::Error("Sim.save_pdb: failed writing to " + path_for_err);
  }

  void write_xyz_frame(std::ostream &os, const std::string &path_for_err) const {
    OpenMM::State st = ctx->getState(OpenMM::State::Positions);
    const auto &pos = st.getPositions();
    os << pos.size() << "\n";
    os << "step=" << total_steps << " time_ps=" << fmt(st.getTime());
    if (spec.has_box) {
      OpenMM::Vec3 a, b, c;
      st.getPeriodicBoxVectors(a, b, c);
      os << " box=" << fmt(a[0]) << "," << fmt(b[1]) << "," << fmt(c[2]);
    }
    os << "\n";
    for (size_t i = 0; i < pos.size(); i++) {
      const std::string &sym = (i < spec.particles.size() && !spec.particles[i].symbol.empty())
                                   ? spec.particles[i].symbol
                                   : "Ar";
      os << sym << " " << fmt(pos[i][0]) << " " << fmt(pos[i][1]) << " " << fmt(pos[i][2]) << "\n";
    }
    os.flush();
    if (!os) throw dslvm::Error("Sim.xyz: failed writing to " + path_for_err);
  }
};

struct Host {
  std::string platform_name = "Reference";
  int threads = 1;
  std::vector<std::unique_ptr<SystemSpec>> systems;
  std::vector<std::unique_ptr<SimObj>> sims;

  template <class T>
  Value hold(std::vector<std::unique_ptr<T>> &arena, const char *type, T v) {
    if (arena.size() >= 1000000) throw dslvm::Error(std::string(type) + " object limit exceeded");
    arena.push_back(std::make_unique<T>(std::move(v)));
    return Value::object(type, arena.back().get());
  }
};

template <class F>
Value translate(F &&fn) {
  try {
    return fn();
  } catch (const dslvm::Error &) {
    throw;
  } catch (const std::exception &e) {
    throw dslvm::Error(e.what());
  }
}

void check_args(const char *who, const std::vector<std::string> &params, Args &a) {
  for (size_t i = 0; i < params.size(); i++) {
    const auto &want = params[i];
    const auto &v = a[i];
    bool ok = want == "float"    ? v.kind == Value::FLOAT
              : want == "int"    ? v.kind == Value::INT
              : want == "string" ? v.kind == Value::STR
              : want == "bool"   ? v.kind == Value::BOOL
                                 : v.kind == Value::OBJ && v.type == want && v.obj;
    if (!ok)
      throw dslvm::Error(std::string(who) + " argument " + std::to_string(i + 1) + " must be " + want);
  }
}

void method(dslvm::Registry &r, const char *type, const char *name, std::vector<std::string> params,
            std::string returns, dslvm::Method cb) {
  std::string who = std::string(type) + "." + name;
  r.method(type, name, params, returns, [who, params, cb](void *self, Args &a) {
    check_args(who.c_str(), params, a);
    return translate([&] { return cb(self, a); });
  });
}

SystemSpec &S(void *p) { return *static_cast<SystemSpec *>(p); }
SimObj &SIM(void *p) { return *static_cast<SimObj *>(p); }

void need_particle(const SystemSpec &s, int64_t i, const char *who) {
  if (i < 0 || i >= (int64_t)s.particles.size())
    throw dslvm::Error(std::string(who) + ": particle index " + std::to_string(i) + " outside 0.." +
                       std::to_string((int64_t)s.particles.size() - 1));
}

void need_sim_particle(const SimObj &sim, int64_t i, const char *who) {
  int64_t n = (int64_t)sim.spec.particles.size();
  if (i < 0 || i >= n)
    throw dslvm::Error(std::string(who) + ": particle index " + std::to_string(i) + " outside 0.." +
                       std::to_string(n - 1));
}

// Bulk builders (fcc, rocksalt, gas, load_xyz) may be called repeatedly on one System: cap the total
// (OpenMM indexes particles with int) and check the budget before growing the particle vector.
constexpr int64_t kMaxParticles = 10000000;
void need_room(SystemSpec &s, int64_t add, const char *who) {
  int64_t total = (int64_t)s.particles.size() + add;
  if (total > kMaxParticles)
    throw dslvm::Error(std::string(who) + ": system would have " + std::to_string(total) +
                       " particles (at most " + std::to_string(kMaxParticles) + ")");
  if ((std::size_t)total > s.particles.capacity()) {
    membudget::require(std::size_t(total) * sizeof(ParticleSpec));
    s.particles.reserve(std::max<std::size_t>(std::size_t(total), 2 * s.particles.capacity()));
  }
}

double uniform01(std::mt19937_64 &rng) {
  return (double)(rng() >> 11) * (1.0 / 9007199254740992.0);
}

int safe_seed(int64_t s) {
  int v = (int)(s & 0x7fffffff);
  return v == 0 ? 1 : v;
}

// Masses (amu) for the elements a PDB coordinate file can name; a System is built from positions
// and masses only, interactions are added by the script.
double element_mass(const std::string &symbol) {
  static const std::map<std::string, double> table = {
      {"H", 1.008},    {"He", 4.0026},  {"Li", 6.94},    {"Be", 9.0122},  {"B", 10.81},    {"C", 12.011},
      {"N", 14.007},   {"O", 15.999},   {"F", 18.998},   {"Ne", 20.180},  {"Na", 22.990},  {"Mg", 24.305},
      {"Al", 26.982},  {"Si", 28.085},  {"P", 30.974},   {"S", 32.06},    {"Cl", 35.45},   {"Ar", 39.948},
      {"K", 39.098},   {"Ca", 40.078},  {"Ti", 47.867},  {"Cr", 51.996},  {"Mn", 54.938},  {"Fe", 55.845},
      {"Co", 58.933},  {"Ni", 58.693},  {"Cu", 63.546},  {"Zn", 65.38},   {"Br", 79.904},  {"Kr", 83.798},
      {"Rb", 85.468},  {"Sr", 87.62},   {"Ag", 107.87},  {"I", 126.90},   {"Xe", 131.29},  {"Cs", 132.91},
      {"Ba", 137.33},  {"Au", 196.97},  {"Hg", 200.59},  {"Pb", 207.2},   {"Se", 78.971},  {"Mo", 95.95},
  };
  std::string s = symbol;
  if (s.empty()) return -1.0;
  s[0] = (char)std::toupper((unsigned char)s[0]);
  for (size_t i = 1; i < s.size(); i++) s[i] = (char)std::tolower((unsigned char)s[i]);
  auto it = table.find(s);
  return it == table.end() ? -1.0 : it->second;
}

std::string pdb_field(const std::string &line, size_t col1, size_t col2) {
  if (line.size() < col1) return "";
  return trim_str(line.substr(col1 - 1, std::min(col2, line.size()) - col1 + 1));
}

// A random rotation matrix (row-major) from a uniformly distributed unit quaternion.
void random_rotation(std::mt19937_64 &rng, double rot[9]) {
  double u1 = uniform01(rng), u2 = uniform01(rng), u3 = uniform01(rng);
  double a = std::sqrt(1.0 - u1), b = std::sqrt(u1);
  double w = a * std::sin(2.0 * kPi * u2), x = a * std::cos(2.0 * kPi * u2);
  double y = b * std::sin(2.0 * kPi * u3), z = b * std::cos(2.0 * kPi * u3);
  rot[0] = 1 - 2 * (y * y + z * z);
  rot[1] = 2 * (x * y - z * w);
  rot[2] = 2 * (x * z + y * w);
  rot[3] = 2 * (x * y + z * w);
  rot[4] = 1 - 2 * (x * x + z * z);
  rot[5] = 2 * (y * z - x * w);
  rot[6] = 2 * (x * z - y * w);
  rot[7] = 2 * (y * z + x * w);
  rot[8] = 1 - 2 * (x * x + y * y);
}

std::unique_ptr<SimObj> build_sim(const SystemSpec &spec, const std::string &platform_name, int threads,
                                  const std::string &integ_name, double dt, double temperature,
                                  double friction, int64_t seed) {
  if (spec.particles.empty())
    throw dslvm::Error("System.sim: system has no particles");
  if (!(dt > 0.0) || !std::isfinite(dt))
    throw dslvm::Error("System.sim: timestep dt must be positive");

  std::string iname = lower_str(trim_str(integ_name));
  if (iname != "verlet" && iname != "langevin")
    throw dslvm::Error("System.sim: unknown integrator '" + integ_name + "' (expected 'verlet' or 'langevin')");
  if (iname == "langevin") {
    if (!(temperature >= 0.0) || !std::isfinite(temperature))
      throw dslvm::Error("System.sim: temperature must be non-negative");
    if (!(friction > 0.0) || !std::isfinite(friction))
      throw dslvm::Error("System.sim: friction must be positive for langevin");
  }

  auto sim = std::make_unique<SimObj>();
  sim->spec = spec;
  sim->sys = std::make_unique<OpenMM::System>();

  if (spec.has_box) {
    sim->sys->setDefaultPeriodicBoxVectors(
        OpenMM::Vec3(spec.box_a, 0.0, 0.0),
        OpenMM::Vec3(0.0, spec.box_b, 0.0),
        OpenMM::Vec3(0.0, 0.0, spec.box_c));
  }

  int mobile = 0;
  std::vector<OpenMM::Vec3> init_pos;
  std::vector<OpenMM::Vec3> init_vel;
  init_pos.reserve(spec.particles.size());
  init_vel.reserve(spec.particles.size());
  for (const auto &p : spec.particles) {
    sim->sys->addParticle(p.mass);
    if (p.mass > 0.0) mobile++;
    init_pos.push_back(p.pos);
    init_vel.push_back(p.vel);
  }
  sim->mobile_particles = mobile;
  for (const auto &c : spec.constraints) {
    if (spec.particles[c.i].mass <= 0.0 || spec.particles[c.j].mass <= 0.0)
      throw dslvm::Error("System.sim: constraint between particles " + std::to_string(c.i) + " and " +
                         std::to_string(c.j) + " involves a fixed (zero-mass) particle");
    sim->sys->addConstraint(c.i, c.j, c.dist);
  }
  sim->dof = 3 * mobile - (int)spec.constraints.size();
  if (sim->dof < 0) throw dslvm::Error("System.sim: more constraints than degrees of freedom");

  if (!spec.bonds.empty()) {
    auto *bf = new OpenMM::HarmonicBondForce();
    bf->setUsesPeriodicBoundaryConditions(spec.has_box);
    for (const auto &b : spec.bonds) bf->addBond(b.i, b.j, b.r0, b.k);
    sim->sys->addForce(bf);
  }

  if (!spec.angles.empty()) {
    auto *af = new OpenMM::HarmonicAngleForce();
    af->setUsesPeriodicBoundaryConditions(spec.has_box);
    for (const auto &ang : spec.angles) af->addAngle(ang.i, ang.j, ang.k, ang.th0, ang.kth);
    sim->sys->addForce(af);
  }

  if (!spec.torsions.empty()) {
    auto *tf = new OpenMM::PeriodicTorsionForce();
    tf->setUsesPeriodicBoundaryConditions(spec.has_box);
    for (const auto &tor : spec.torsions)
      tf->addTorsion(tor.i, tor.j, tor.k, tor.l, tor.periodicity, tor.phase, tor.k_torsion);
    sim->sys->addForce(tf);
  }

  if (spec.has_lj || spec.has_charges || spec.has_coulomb) {
    auto *nb = new OpenMM::NonbondedForce();
    // One NonbondedForce carries both LJ and Coulomb, so they must agree on the cutoff.
    if (spec.has_lj && spec.lj_cutoff > 0.0 && spec.has_coulomb) {
      if (spec.coulomb_method == "none" || spec.coulomb_method == "nocutoff")
        throw dslvm::Error("System.sim: lj cutoff " + fmt(spec.lj_cutoff) +
                           " conflicts with coulomb('none'): LJ and Coulomb share one cutoff");
      if (spec.coulomb_cutoff != spec.lj_cutoff)
        throw dslvm::Error("System.sim: lj cutoff " + fmt(spec.lj_cutoff) + " differs from coulomb cutoff " +
                           fmt(spec.coulomb_cutoff) + ": LJ and Coulomb share one cutoff");
    }
    double cutoff = spec.coulomb_cutoff > 0.0 ? spec.coulomb_cutoff : spec.lj_cutoff;
    OpenMM::NonbondedForce::NonbondedMethod nb_method = OpenMM::NonbondedForce::NoCutoff;

    if (spec.has_coulomb) {
      if (spec.coulomb_method == "none" || spec.coulomb_method == "nocutoff") {
        nb_method = OpenMM::NonbondedForce::NoCutoff;
      } else if (spec.coulomb_method == "cutoff") {
        if (!(cutoff > 0.0))
          throw dslvm::Error("System.sim: cutoff method requires positive cutoff distance");
        nb_method = spec.has_box ? OpenMM::NonbondedForce::CutoffPeriodic
                                 : OpenMM::NonbondedForce::CutoffNonPeriodic;
      } else if (spec.coulomb_method == "ewald") {
        if (!spec.has_box)
          throw dslvm::Error("System.sim: ewald requires a periodic box");
        if (!(cutoff > 0.0))
          throw dslvm::Error("System.sim: ewald requires a positive cutoff distance");
        nb_method = OpenMM::NonbondedForce::Ewald;
      } else if (spec.coulomb_method == "pme") {
        if (!spec.has_box)
          throw dslvm::Error("System.sim: pme requires a periodic box");
        if (!(cutoff > 0.0))
          throw dslvm::Error("System.sim: pme requires a positive cutoff distance");
        nb_method = OpenMM::NonbondedForce::PME;
      }
    } else if (cutoff > 0.0) {
      nb_method = spec.has_box ? OpenMM::NonbondedForce::CutoffPeriodic
                               : OpenMM::NonbondedForce::CutoffNonPeriodic;
    }

    if (nb_method == OpenMM::NonbondedForce::CutoffPeriodic ||
        nb_method == OpenMM::NonbondedForce::Ewald ||
        nb_method == OpenMM::NonbondedForce::PME) {
      double min_box = std::min({spec.box_a, spec.box_b, spec.box_c});
      if (cutoff >= 0.5 * min_box)
        throw dslvm::Error("System.sim: cutoff (" + fmt(cutoff) +
                           ") must be less than half the periodic box size (" + fmt(0.5 * min_box) + ")");
    }

    nb->setNonbondedMethod(nb_method);
    if (cutoff > 0.0) nb->setCutoffDistance(cutoff);
    nb->setReactionFieldDielectric(spec.rf_dielectric);
    nb->setEwaldErrorTolerance(1e-5);
    nb->setUseDispersionCorrection(false);

    for (const auto &p : spec.particles) {
      double eps = p.has_lj ? p.lj_eps : (spec.has_lj ? spec.default_lj_eps : 0.0);
      double sig = p.has_lj ? p.lj_sigma : (spec.has_lj ? spec.default_lj_sigma : 0.1);
      nb->addParticle(p.charge, sig, eps);
    }
    if (nb_method == OpenMM::NonbondedForce::PME) {
      // ReferencePME (both platforms) mallocs the complex grid and per-particle B-spline tables at
      // every force evaluation and dereferences them unchecked: size them here and count them for
      // the life of the Sim, so an oversized grid (big box, small cutoff) is a run error.
      double alpha = 0.0;
      int nx = 0, ny = 0, nz = 0;
      OpenMM::NonbondedForceImpl::calcPMEParameters(*sim->sys, *nb, alpha, nx, ny, nz, false);
      double cells = (double)nx * (double)ny * (double)nz;
      if (cells > 2147483647.0)
        throw dslvm::Error("System.sim: PME grid " + std::to_string(nx) + "x" + std::to_string(ny) + "x" +
                           std::to_string(nz) + " is too large (use a larger cutoff or a smaller box)");
      std::size_t n = spec.particles.size();
      std::size_t bytes = std::size_t(cells) * 16 + n * (2 * 3 * 5 * 8 + 3 * 8 + 3 * 4) +
                          std::size_t(nx + ny + nz) * 8;
      membudget::charge(bytes);
      sim->charged = bytes;
    }
    for (const auto &ex : spec.exclusions) {
      nb->addException(ex.first, ex.second, 0.0, 0.1, 0.0, true);
    }
    sim->sys->addForce(nb);
  }

  for (const auto &ce : spec.custom_externals) {
    auto *f = new OpenMM::CustomExternalForce(ce.expr);
    f->addGlobalParameter("pi", 3.14159265358979323846);
    f->addGlobalParameter("e", 2.71828182845904523536);
    for (const auto &kv : ce.params) f->addGlobalParameter(kv.first, kv.second);
    for (size_t i = 0; i < spec.particles.size(); i++) f->addParticle((int)i, {});
    sim->sys->addForce(f);
  }

  for (const auto &cb : spec.custom_bonds) {
    auto *f = new OpenMM::CustomBondForce(cb.expr);
    f->setUsesPeriodicBoundaryConditions(spec.has_box);
    f->addGlobalParameter("pi", 3.14159265358979323846);
    f->addGlobalParameter("e", 2.71828182845904523536);
    f->addBond(cb.i, cb.j, {});
    sim->sys->addForce(f);
  }

  for (const auto &cnb : spec.custom_nonbondeds) {
    auto *f = new OpenMM::CustomNonbondedForce(cnb.expr);
    f->addGlobalParameter("pi", 3.14159265358979323846);
    f->addGlobalParameter("e", 2.71828182845904523536);
    for (const auto &kv : cnb.params) f->addGlobalParameter(kv.first, kv.second);
    if (cnb.cutoff > 0.0) {
      if (spec.has_box) {
        double min_box = std::min({spec.box_a, spec.box_b, spec.box_c});
        if (cnb.cutoff >= 0.5 * min_box)
          throw dslvm::Error("System.sim: custom_nonbonded cutoff must be less than half the box size");
        f->setNonbondedMethod(OpenMM::CustomNonbondedForce::CutoffPeriodic);
      } else {
        f->setNonbondedMethod(OpenMM::CustomNonbondedForce::CutoffNonPeriodic);
      }
      f->setCutoffDistance(cnb.cutoff);
    } else {
      f->setNonbondedMethod(OpenMM::CustomNonbondedForce::NoCutoff);
    }
    f->setUseLongRangeCorrection(false);
    for (size_t i = 0; i < spec.particles.size(); i++) f->addParticle({});
    for (const auto &ex : spec.exclusions) f->addExclusion(ex.first, ex.second);
    sim->sys->addForce(f);
  }

  if (spec.has_barostat) {
    if (!spec.has_box)
      throw dslvm::Error("System.sim: barostat requires a periodic box");
    auto *baro = new OpenMM::MonteCarloBarostat(spec.baro_pressure, spec.baro_temp, spec.baro_freq);
    baro->setRandomNumberSeed(safe_seed(spec.baro_seed));
    sim->sys->addForce(baro);
  }

  if (spec.has_andersen) {
    auto *at = new OpenMM::AndersenThermostat(spec.andersen_temp, spec.andersen_rate);
    at->setRandomNumberSeed(safe_seed(spec.andersen_seed));
    sim->sys->addForce(at);
  }

  int iseed = safe_seed(seed);
  // Reset Reference platform's cached Box-Muller spare normal value before initializing Context.
  OpenMM::SimTKOpenMMUtilities::setRandomNumberSeed((uint32_t)iseed);
  (void)OpenMM::SimTKOpenMMUtilities::getUniformlyDistributedRandomNumber();
  OpenMM::SimTKOpenMMUtilities::setRandomNumberSeed((uint32_t)iseed);

  if (iname == "verlet") {
    sim->integ = std::make_unique<OpenMM::VerletIntegrator>(dt);
  } else {
    auto lm = std::make_unique<OpenMM::LangevinMiddleIntegrator>(temperature, friction, dt);
    lm->setRandomNumberSeed(iseed);
    sim->integ = std::move(lm);
  }

  OpenMM::Platform &plat = OpenMM::Platform::getPlatformByName(platform_name);
  std::map<std::string, std::string> props;
  if (platform_name == "CPU") {
    props["Threads"] = std::to_string(threads);
    props["DeterministicForces"] = "true";
  }
  sim->ctx = std::make_unique<OpenMM::Context>(*sim->sys, *sim->integ, plat, props);
  sim->ctx->setPositions(init_pos);
  sim->ctx->setVelocities(init_vel);
  if (!spec.constraints.empty()) {
    // Start on the constraint manifold even when the script's coordinates are only approximate.
    sim->ctx->applyConstraints(1e-6);
    sim->ctx->applyVelocityConstraints(1e-6);
  }
  return sim;
}

void register_host(dslvm::Registry &r, Host &h) {
  r.context("md", "Md");

  // ---------------------------------------------------------------- Md
  method(r, "Md", "system", {}, "System", [&h](void *, Args &) {
    return h.hold(h.systems, "System", SystemSpec{});
  });
  method(r, "Md", "platform", {"string"}, "Md", [&h](void *p, Args &a) {
    std::string s = lower_str(trim_str(a[0].s));
    if (s == "reference") {
      h.platform_name = "Reference";
    } else if (s == "cpu") {
      bool found = false;
      for (int i = 0; i < OpenMM::Platform::getNumPlatforms(); i++) {
        if (OpenMM::Platform::getPlatform(i).getName() == "CPU") found = true;
      }
      if (!found) throw dslvm::Error("Md.platform: CPU platform is not supported on this processor");
      h.platform_name = "CPU";
    } else {
      throw dslvm::Error("Md.platform: unknown platform '" + a[0].s + "' (expected 'reference' or 'cpu')");
    }
    return Value::object("Md", p);
  });
  method(r, "Md", "threads", {"int"}, "Md", [&h](void *p, Args &a) {
    if (a[0].i < 1 || a[0].i > 256) throw dslvm::Error("Md.threads: thread count must be in 1..256");
    h.threads = (int)a[0].i;
    return Value::object("Md", p);
  });
  method(r, "Md", "eval", {"string"}, "float", [](void *, Args &a) {
    std::map<std::string, double> consts = {
        {"pi", 3.14159265358979323846},
        {"e", 2.71828182845904523536},
        {"kB", BOLTZ},
        {"one_4pi_eps0", ONE_4PI_EPS0},
    };
    std::set<std::string> vars;
    Lepton::ParsedExpression parsed = parse_lepton(a[0].s, vars, "Md.eval");
    for (const auto &v : vars) {
      if (consts.find(v) == consts.end())
        throw dslvm::Error("Md.eval: expression \"" + a[0].s + "\": unknown name '" + v + "'");
    }
    double v = parsed.evaluate(consts);
    if (!std::isfinite(v))
      throw dslvm::Error("Md.eval: expression \"" + a[0].s + "\" is not finite (" + fmt(v) + ")");
    return Value::real(v);
  });
  method(r, "Md", "assert_close", {"float", "float", "float"}, "void", [](void *, Args &a) {
    if (!std::isfinite(a[2].f) || a[2].f < 0.0)
      throw dslvm::Error("assert_close: tolerance must be non-negative and finite");
    if (!std::isfinite(a[0].f) || !std::isfinite(a[1].f) || !(std::fabs(a[0].f - a[1].f) <= a[2].f))
      throw dslvm::Error("assert_close failed: " + fmt(a[0].f) + " vs " + fmt(a[1].f) +
                         " (tolerance " + fmt(a[2].f) + ")");
    return Value::none();
  });
  method(r, "Md", "print", {"string"}, "void", [](void *, Args &a) {
    std::cout << a[0].s << std::endl;
    return Value::none();
  });
  method(r, "Md", "version", {}, "string", [](void *, Args &) {
    return Value::string("lmmolecule " LMMOLECULE_VERSION);
  });

  // ---------------------------------------------------------------- System
  method(r, "System", "box", {"float", "float", "float"}, "System", [](void *p, Args &a) {
    if (!(a[0].f > 0.0 && a[1].f > 0.0 && a[2].f > 0.0) ||
        !std::isfinite(a[0].f) || !std::isfinite(a[1].f) || !std::isfinite(a[2].f))
      throw dslvm::Error("System.box: box dimensions must be positive");
    S(p).has_box = true;
    S(p).box_a = a[0].f;
    S(p).box_b = a[1].f;
    S(p).box_c = a[2].f;
    return Value::object("System", p);
  });
  method(r, "System", "particle", {"float"}, "int", [](void *p, Args &a) {
    if (!(a[0].f >= 0.0) || !std::isfinite(a[0].f))
      throw dslvm::Error("System.particle: mass must be non-negative");
    if (S(p).particles.size() >= 100000)
      throw dslvm::Error("System.particle: particle count limit exceeded");
    ParticleSpec ps;
    ps.mass = a[0].f;
    int idx = (int)S(p).particles.size();
    S(p).particles.push_back(ps);
    return Value::integer(idx);
  });
  method(r, "System", "add_particle", {"float", "float", "float", "float"}, "System", [](void *p, Args &a) {
    if (!(a[0].f >= 0.0) || !std::isfinite(a[0].f))
      throw dslvm::Error("System.add_particle: mass must be non-negative");
    if (!std::isfinite(a[1].f) || !std::isfinite(a[2].f) || !std::isfinite(a[3].f))
      throw dslvm::Error("System.add_particle: coordinates must be finite");
    if (S(p).particles.size() >= 100000)
      throw dslvm::Error("System.add_particle: particle count limit exceeded");
    ParticleSpec ps;
    ps.mass = a[0].f;
    ps.pos = OpenMM::Vec3(a[1].f, a[2].f, a[3].f);
    S(p).particles.push_back(ps);
    return Value::object("System", p);
  });
  method(r, "System", "count", {}, "int", [](void *p, Args &) {
    return Value::integer((int64_t)S(p).particles.size());
  });
  method(r, "System", "fcc", {"int", "float", "float"}, "System", [](void *p, Args &a) {
    int64_t n = a[0].i;
    double lat = a[1].f;
    double mass = a[2].f;
    if (n < 1 || n > 30) throw dslvm::Error("System.fcc: cell count n must be in 1..30");
    if (!(lat > 0.0) || !std::isfinite(lat)) throw dslvm::Error("System.fcc: lattice constant a must be positive");
    if (!(mass >= 0.0) || !std::isfinite(mass)) throw dslvm::Error("System.fcc: mass must be non-negative");
    const double basis[4][3] = {
        {0.0, 0.0, 0.0},
        {0.0, 0.5, 0.5},
        {0.5, 0.0, 0.5},
        {0.5, 0.5, 0.0},
    };
    need_room(S(p), 4 * n * n * n, "System.fcc");
    double L = (double)n * lat;
    S(p).has_box = true;
    S(p).box_a = L;
    S(p).box_b = L;
    S(p).box_c = L;
    for (int64_t ix = 0; ix < n; ix++) {
      for (int64_t iy = 0; iy < n; iy++) {
        for (int64_t iz = 0; iz < n; iz++) {
          for (int b = 0; b < 4; b++) {
            ParticleSpec ps;
            ps.mass = mass;
            ps.pos = OpenMM::Vec3(((double)ix + basis[b][0]) * lat,
                                  ((double)iy + basis[b][1]) * lat,
                                  ((double)iz + basis[b][2]) * lat);
            S(p).particles.push_back(ps);
          }
        }
      }
    }
    return Value::object("System", p);
  });
  method(r, "System", "rocksalt", {"int", "float", "float", "float", "float"}, "System", [](void *p, Args &a) {
    int64_t n = a[0].i;
    double lat = a[1].f;
    double m_pos = a[2].f;
    double m_neg = a[3].f;
    double q = a[4].f;
    if (n < 1 || n > 20) throw dslvm::Error("System.rocksalt: cell count n must be in 1..20");
    if (!(lat > 0.0) || !std::isfinite(lat)) throw dslvm::Error("System.rocksalt: lattice constant a must be positive");
    if (!(m_pos >= 0.0 && m_neg >= 0.0) || !std::isfinite(m_pos) || !std::isfinite(m_neg))
      throw dslvm::Error("System.rocksalt: masses must be non-negative");
    if (!std::isfinite(q)) throw dslvm::Error("System.rocksalt: charge must be finite");
    const double basis[8][3] = {
        {0.0, 0.0, 0.0}, {0.0, 0.5, 0.5}, {0.5, 0.0, 0.5}, {0.5, 0.5, 0.0},
        {0.5, 0.0, 0.0}, {0.0, 0.5, 0.0}, {0.0, 0.0, 0.5}, {0.5, 0.5, 0.5},
    };
    need_room(S(p), 8 * n * n * n, "System.rocksalt");
    double L = (double)n * lat;
    S(p).has_box = true;
    S(p).box_a = L;
    S(p).box_b = L;
    S(p).box_c = L;
    S(p).has_charges = true;
    for (int64_t ix = 0; ix < n; ix++) {
      for (int64_t iy = 0; iy < n; iy++) {
        for (int64_t iz = 0; iz < n; iz++) {
          for (int b = 0; b < 8; b++) {
            ParticleSpec ps;
            ps.mass = (b < 4) ? m_pos : m_neg;
            ps.charge = (b < 4) ? q : -q;
            ps.symbol = (b < 4) ? "Na" : "Cl";
            ps.pos = OpenMM::Vec3(((double)ix + basis[b][0]) * lat,
                                  ((double)iy + basis[b][1]) * lat,
                                  ((double)iz + basis[b][2]) * lat);
            S(p).particles.push_back(ps);
          }
        }
      }
    }
    return Value::object("System", p);
  });
  method(r, "System", "gas", {"int", "float", "float", "int"}, "System", [](void *p, Args &a) {
    int64_t n = a[0].i;
    double box = a[1].f;
    double mass = a[2].f;
    int64_t seed = a[3].i;
    if (n < 1 || n > 50000) throw dslvm::Error("System.gas: particle count n must be in 1..50000");
    if (!(box > 0.0) || !std::isfinite(box)) throw dslvm::Error("System.gas: box size must be positive");
    if (!(mass >= 0.0) || !std::isfinite(mass)) throw dslvm::Error("System.gas: mass must be non-negative");
    need_room(S(p), n, "System.gas");
    S(p).has_box = true;
    S(p).box_a = box;
    S(p).box_b = box;
    S(p).box_c = box;
    std::mt19937_64 rng((uint64_t)seed);
    double min_dist = 0.35 * box / std::cbrt((double)n);
    double min_dist2 = min_dist * min_dist;
    // Every particle already in the system (earlier gas/lattice calls included) is an obstacle.
    for (int64_t i = 0; i < n; i++) {
      OpenMM::Vec3 cand;
      bool placed = false;
      for (int attempt = 0; attempt < 200 && !placed; attempt++) {
        cand = OpenMM::Vec3(uniform01(rng) * box, uniform01(rng) * box, uniform01(rng) * box);
        placed = true;
        for (size_t j = 0; j < S(p).particles.size(); j++) {
          double dx = cand[0] - S(p).particles[j].pos[0];
          double dy = cand[1] - S(p).particles[j].pos[1];
          double dz = cand[2] - S(p).particles[j].pos[2];
          dx -= std::round(dx / box) * box;
          dy -= std::round(dy / box) * box;
          dz -= std::round(dz / box) * box;
          if (dx * dx + dy * dy + dz * dz < min_dist2) {
            placed = false;
            break;
          }
        }
      }
      if (!placed)
        throw dslvm::Error("System.gas: could not place particle " + std::to_string(i) + " of " + std::to_string(n) +
                           " at least " + fmt(min_dist) + " nm from the " + std::to_string(S(p).particles.size()) +
                           " existing particles after 200 attempts (box too crowded)");
      ParticleSpec ps;
      ps.mass = mass;
      ps.pos = cand;
      S(p).particles.push_back(ps);
    }
    return Value::object("System", p);
  });
  method(r, "System", "load_xyz", {"string", "float"}, "System", [](void *p, Args &a) {
    double mass = a[1].f;
    if (!(mass >= 0.0) || !std::isfinite(mass))
      throw dslvm::Error("System.load_xyz: mass must be non-negative");
    std::ifstream f(a[0].s);
    if (!f) throw dslvm::Error("System.load_xyz: cannot open " + a[0].s);
    std::string line1, comment;
    if (!std::getline(f, line1) || !std::getline(f, comment))
      throw dslvm::Error("System.load_xyz: invalid XYZ header in " + a[0].s);
    int n = 0;
    try {
      size_t used = 0;
      std::string t = trim_str(line1);
      n = std::stoi(t, &used);
      if (used != t.size() || n < 1 || n > 100000) throw std::runtime_error("bad count");
    } catch (...) {
      throw dslvm::Error("System.load_xyz: bad atom count in " + a[0].s);
    }
    need_room(S(p), n, "System.load_xyz");
    for (int i = 0; i < n; i++) {
      std::string line;
      if (!std::getline(f, line))
        throw dslvm::Error("System.load_xyz: unexpected EOF at atom " + std::to_string(i) + " in " + a[0].s);
      std::istringstream iss(line);
      std::string sym;
      double x, y, z;
      if (!(iss >> sym >> x >> y >> z) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        throw dslvm::Error("System.load_xyz: malformed coordinate line at atom " + std::to_string(i));
      ParticleSpec ps;
      ps.mass = mass;
      ps.symbol = sym;
      ps.pos = OpenMM::Vec3(x, y, z);
      S(p).particles.push_back(ps);
    }
    return Value::object("System", p);
  });
  method(r, "System", "load_pdb", {"string"}, "System", [](void *p, Args &a) {
    std::ifstream f(a[0].s);
    if (!f) throw dslvm::Error("System.load_pdb: cannot open " + a[0].s);
    std::string line;
    int lineno = 0, added = 0;
    while (std::getline(f, line)) {
      lineno++;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::string rec = line.substr(0, std::min<size_t>(6, line.size()));
      if (rec == "END" || rec == "ENDMDL") break;
      if (rec == "CRYST1") {
        double a_ = std::atof(pdb_field(line, 7, 15).c_str()), b_ = std::atof(pdb_field(line, 16, 24).c_str()),
               c_ = std::atof(pdb_field(line, 25, 33).c_str());
        if (a_ > 0.0 && b_ > 0.0 && c_ > 0.0 && std::isfinite(a_ + b_ + c_)) {
          S(p).has_box = true;
          S(p).box_a = a_ * 0.1;
          S(p).box_b = b_ * 0.1;
          S(p).box_c = c_ * 0.1;
        }
        continue;
      }
      if (rec != "ATOM  " && rec != "HETATM") continue;
      need_room(S(p), 1, "System.load_pdb");
      std::string xs = pdb_field(line, 31, 38), ys = pdb_field(line, 39, 46), zs = pdb_field(line, 47, 54);
      size_t ux = 0, uy = 0, uz = 0;
      double x = 0, y = 0, z = 0;
      try {
        x = std::stod(xs, &ux);
        y = std::stod(ys, &uy);
        z = std::stod(zs, &uz);
      } catch (...) {
        ux = uy = uz = 0;
      }
      if (xs.empty() || ys.empty() || zs.empty() || ux != xs.size() || uy != ys.size() || uz != zs.size() ||
          !std::isfinite(x + y + z))
        throw dslvm::Error("System.load_pdb: bad coordinates at line " + std::to_string(lineno) + " of " + a[0].s);
      std::string element = pdb_field(line, 77, 78);
      if (element.empty()) {
        // Derive the element from the atom name: a name starting in column 13 has a two-letter element.
        std::string name = line.size() >= 16 ? line.substr(12, 4) : "";
        std::string letters;
        for (char c : name) {
          if (std::isalpha((unsigned char)c)) letters += c;
          else if (!letters.empty()) break;
        }
        if (!letters.empty()) element = (name[0] != ' ' && letters.size() >= 2) ? letters.substr(0, 2) : letters.substr(0, 1);
      }
      double mass = element_mass(element);
      if (mass < 0.0)
        throw dslvm::Error("System.load_pdb: unknown element '" + element + "' at line " + std::to_string(lineno) +
                           " of " + a[0].s);
      ParticleSpec ps;
      ps.mass = mass;
      ps.symbol = element;
      ps.symbol[0] = (char)std::toupper((unsigned char)ps.symbol[0]);
      for (size_t i = 1; i < ps.symbol.size(); i++) ps.symbol[i] = (char)std::tolower((unsigned char)ps.symbol[i]);
      ps.pos = OpenMM::Vec3(x * 0.1, y * 0.1, z * 0.1);
      S(p).particles.push_back(ps);
      added++;
    }
    if (added == 0) throw dslvm::Error("System.load_pdb: no ATOM/HETATM records in " + a[0].s);
    return Value::object("System", p);
  });
  method(r, "System", "water", {"string", "float", "float", "float"}, "System", [](void *p, Args &a) {
    const WaterModel *w = water_model(a[0].s, "System.water");
    if (!std::isfinite(a[1].f) || !std::isfinite(a[2].f) || !std::isfinite(a[3].f))
      throw dslvm::Error("System.water: coordinates must be finite");
    need_room(S(p), 3, "System.water");
    const double identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    S(p).add_water(*w, OpenMM::Vec3(a[1].f, a[2].f, a[3].f), identity);
    return Value::object("System", p);
  });
  method(r, "System", "water_box", {"string", "int", "float", "int"}, "System", [](void *p, Args &a) {
    const WaterModel *w = water_model(a[0].s, "System.water_box");
    int64_t n = a[1].i;
    double spacing = a[2].f;
    if (n < 1 || n > 30) throw dslvm::Error("System.water_box: molecules per side n must be in 1..30");
    if (!(spacing > 0.0) || !std::isfinite(spacing))
      throw dslvm::Error("System.water_box: spacing must be positive (0.31 nm is liquid density)");
    if (spacing < 2.0 * w->r_oh)
      throw dslvm::Error("System.water_box: spacing " + fmt(spacing) + " nm is below the molecule size");
    need_room(S(p), 3 * n * n * n, "System.water_box");
    double L = (double)n * spacing;
    S(p).has_box = true;
    S(p).box_a = L;
    S(p).box_b = L;
    S(p).box_c = L;
    std::mt19937_64 rng((uint64_t)a[3].i);
    for (int64_t ix = 0; ix < n; ix++) {
      for (int64_t iy = 0; iy < n; iy++) {
        for (int64_t iz = 0; iz < n; iz++) {
          double rot[9];
          random_rotation(rng, rot);
          S(p).add_water(*w, OpenMM::Vec3(((double)ix + 0.5) * spacing, ((double)iy + 0.5) * spacing,
                                          ((double)iz + 0.5) * spacing), rot);
        }
      }
    }
    return Value::object("System", p);
  });
  method(r, "System", "constrain", {"int", "int", "float"}, "System", [](void *p, Args &a) {
    need_particle(S(p), a[0].i, "System.constrain");
    need_particle(S(p), a[1].i, "System.constrain");
    if (a[0].i == a[1].i) throw dslvm::Error("System.constrain: cannot constrain a particle to itself");
    if (!(a[2].f > 0.0) || !std::isfinite(a[2].f)) throw dslvm::Error("System.constrain: distance must be positive");
    S(p).constraints.push_back(ConstraintSpec{(int)a[0].i, (int)a[1].i, a[2].f});
    return Value::object("System", p);
  });
  method(r, "System", "pos", {"int", "float", "float", "float"}, "System", [](void *p, Args &a) {
    need_particle(S(p), a[0].i, "System.pos");
    if (!std::isfinite(a[1].f) || !std::isfinite(a[2].f) || !std::isfinite(a[3].f))
      throw dslvm::Error("System.pos: coordinates must be finite");
    S(p).particles[a[0].i].pos = OpenMM::Vec3(a[1].f, a[2].f, a[3].f);
    return Value::object("System", p);
  });
  method(r, "System", "vel", {"int", "float", "float", "float"}, "System", [](void *p, Args &a) {
    need_particle(S(p), a[0].i, "System.vel");
    if (!std::isfinite(a[1].f) || !std::isfinite(a[2].f) || !std::isfinite(a[3].f))
      throw dslvm::Error("System.vel: velocities must be finite");
    S(p).particles[a[0].i].vel = OpenMM::Vec3(a[1].f, a[2].f, a[3].f);
    return Value::object("System", p);
  });
  method(r, "System", "bond", {"int", "int", "float", "float"}, "System", [](void *p, Args &a) {
    need_particle(S(p), a[0].i, "System.bond");
    need_particle(S(p), a[1].i, "System.bond");
    if (a[0].i == a[1].i) throw dslvm::Error("System.bond: cannot bond a particle to itself");
    if (!(a[2].f >= 0.0) || !std::isfinite(a[2].f) || !(a[3].f >= 0.0) || !std::isfinite(a[3].f))
      throw dslvm::Error("System.bond: r0 and k must be non-negative");
    S(p).bonds.push_back(BondSpec{(int)a[0].i, (int)a[1].i, a[2].f, a[3].f});
    return Value::object("System", p);
  });
  method(r, "System", "angle", {"int", "int", "int", "float", "float"}, "System", [](void *p, Args &a) {
    need_particle(S(p), a[0].i, "System.angle");
    need_particle(S(p), a[1].i, "System.angle");
    need_particle(S(p), a[2].i, "System.angle");
    if (a[0].i == a[1].i || a[1].i == a[2].i || a[0].i == a[2].i)
      throw dslvm::Error("System.angle: angle particles must be distinct");
    if (!std::isfinite(a[3].f) || !(a[4].f >= 0.0) || !std::isfinite(a[4].f))
      throw dslvm::Error("System.angle: invalid angle parameters");
    S(p).angles.push_back(AngleSpec{(int)a[0].i, (int)a[1].i, (int)a[2].i, a[3].f, a[4].f});
    return Value::object("System", p);
  });
  method(r, "System", "torsion", {"int", "int", "int", "int", "int", "float", "float"}, "System", [](void *p, Args &a) {
    need_particle(S(p), a[0].i, "System.torsion");
    need_particle(S(p), a[1].i, "System.torsion");
    need_particle(S(p), a[2].i, "System.torsion");
    need_particle(S(p), a[3].i, "System.torsion");
    if (a[0].i == a[1].i || a[1].i == a[2].i || a[2].i == a[3].i ||
        a[0].i == a[2].i || a[0].i == a[3].i || a[1].i == a[3].i)
      throw dslvm::Error("System.torsion: torsion particles must be distinct");
    if (a[4].i < 1) throw dslvm::Error("System.torsion: periodicity must be positive");
    if (!std::isfinite(a[5].f) || !std::isfinite(a[6].f))
      throw dslvm::Error("System.torsion: phase and k must be finite");
    S(p).torsions.push_back(
        TorsionSpec{(int)a[0].i, (int)a[1].i, (int)a[2].i, (int)a[3].i, (int)a[4].i, a[5].f, a[6].f});
    return Value::object("System", p);
  });
  method(r, "System", "lj", {"float", "float", "float"}, "System", [](void *p, Args &a) {
    if (!(a[0].f >= 0.0) || !std::isfinite(a[0].f))
      throw dslvm::Error("System.lj: epsilon must be non-negative");
    if (!(a[1].f > 0.0) || !std::isfinite(a[1].f))
      throw dslvm::Error("System.lj: sigma must be positive");
    if (!std::isfinite(a[2].f) || a[2].f < 0.0)
      throw dslvm::Error("System.lj: cutoff must be non-negative");
    S(p).has_lj = true;
    S(p).default_lj_eps = a[0].f;
    S(p).default_lj_sigma = a[1].f;
    S(p).lj_cutoff = a[2].f;
    return Value::object("System", p);
  });
  method(r, "System", "lj_particle", {"int", "float", "float"}, "System", [](void *p, Args &a) {
    need_particle(S(p), a[0].i, "System.lj_particle");
    if (!(a[1].f >= 0.0) || !std::isfinite(a[1].f) || !(a[2].f > 0.0) || !std::isfinite(a[2].f))
      throw dslvm::Error("System.lj_particle: invalid eps/sigma");
    S(p).has_lj = true;
    S(p).particles[a[0].i].has_lj = true;
    S(p).particles[a[0].i].lj_eps = a[1].f;
    S(p).particles[a[0].i].lj_sigma = a[2].f;
    return Value::object("System", p);
  });
  method(r, "System", "charge", {"int", "float"}, "System", [](void *p, Args &a) {
    need_particle(S(p), a[0].i, "System.charge");
    if (!std::isfinite(a[1].f)) throw dslvm::Error("System.charge: charge must be finite");
    S(p).has_charges = true;
    S(p).particles[a[0].i].charge = a[1].f;
    return Value::object("System", p);
  });
  method(r, "System", "coulomb", {"string", "float"}, "System", [](void *p, Args &a) {
    std::string m = lower_str(trim_str(a[0].s));
    // 'cutoff' is OpenMM's reaction-field truncation (see System.dielectric); the explicit name is an alias.
    if (m == "reaction_field" || m == "reactionfield") m = "cutoff";
    if (m != "none" && m != "nocutoff" && m != "cutoff" && m != "ewald" && m != "pme")
      throw dslvm::Error("System.coulomb: unknown method '" + a[0].s +
                         "' (expected 'none', 'cutoff' (= 'reaction_field'), 'ewald', or 'pme')");
    if (!std::isfinite(a[1].f) || a[1].f < 0.0)
      throw dslvm::Error("System.coulomb: cutoff must be non-negative");
    if ((m == "cutoff" || m == "ewald" || m == "pme") && !(a[1].f > 0.0))
      throw dslvm::Error("System.coulomb: method '" + m + "' requires positive cutoff");
    S(p).has_coulomb = true;
    S(p).coulomb_method = m;
    S(p).coulomb_cutoff = a[1].f;
    return Value::object("System", p);
  });
  method(r, "System", "dielectric", {"float"}, "System", [](void *p, Args &a) {
    if (!std::isfinite(a[0].f) || !(a[0].f >= 1.0))
      throw dslvm::Error("System.dielectric: reaction-field dielectric must be >= 1 (1 = plain shifted cutoff)");
    S(p).rf_dielectric = a[0].f;
    return Value::object("System", p);
  });
  method(r, "System", "exclude", {"int", "int"}, "System", [](void *p, Args &a) {
    need_particle(S(p), a[0].i, "System.exclude");
    need_particle(S(p), a[1].i, "System.exclude");
    if (a[0].i == a[1].i) throw dslvm::Error("System.exclude: particle indices must be distinct");
    int u = (int)std::min(a[0].i, a[1].i);
    int v = (int)std::max(a[0].i, a[1].i);
    S(p).exclusions.insert({u, v});
    return Value::object("System", p);
  });
  method(r, "System", "custom_external", {"string", "string"}, "System", [](void *p, Args &a) {
    auto params = parse_params(a[1].s, "System.custom_external");
    std::set<std::string> allowed = {"x", "y", "z"};
    for (const auto &kv : params) allowed.insert(kv.first);
    validate_lepton(a[0].s, allowed, "System.custom_external");
    S(p).custom_externals.push_back(CustomExternalSpec{a[0].s, std::move(params)});
    return Value::object("System", p);
  });
  method(r, "System", "custom_bond", {"int", "int", "string"}, "System", [](void *p, Args &a) {
    need_particle(S(p), a[0].i, "System.custom_bond");
    need_particle(S(p), a[1].i, "System.custom_bond");
    if (a[0].i == a[1].i) throw dslvm::Error("System.custom_bond: cannot bond a particle to itself");
    validate_lepton(a[2].s, {"r"}, "System.custom_bond");
    S(p).custom_bonds.push_back(CustomBondSpec{(int)a[0].i, (int)a[1].i, a[2].s});
    return Value::object("System", p);
  });
  method(r, "System", "custom_nonbonded", {"string", "string", "float"}, "System", [](void *p, Args &a) {
    if (!std::isfinite(a[2].f) || a[2].f < 0.0)
      throw dslvm::Error("System.custom_nonbonded: cutoff must be non-negative");
    auto params = parse_params(a[1].s, "System.custom_nonbonded");
    std::set<std::string> allowed = {"r"};
    for (const auto &kv : params) allowed.insert(kv.first);
    validate_lepton(a[0].s, allowed, "System.custom_nonbonded");
    S(p).custom_nonbondeds.push_back(CustomNonbondedSpec{a[0].s, std::move(params), a[2].f});
    return Value::object("System", p);
  });
  method(r, "System", "barostat", {"float", "float", "int", "int"}, "System", [](void *p, Args &a) {
    if (!std::isfinite(a[0].f)) throw dslvm::Error("System.barostat: pressure must be finite");
    if (!(a[1].f > 0.0) || !std::isfinite(a[1].f))
      throw dslvm::Error("System.barostat: temperature must be positive (the Monte Carlo acceptance uses kT)");
    if (a[2].i < 1 || a[2].i > 1000000) throw dslvm::Error("System.barostat: frequency must be in 1..1000000 steps");
    S(p).has_barostat = true;
    S(p).baro_pressure = a[0].f;
    S(p).baro_temp = a[1].f;
    S(p).baro_freq = (int)a[2].i;
    S(p).baro_seed = (int)a[3].i;
    return Value::object("System", p);
  });
  method(r, "System", "andersen", {"float", "float", "int"}, "System", [](void *p, Args &a) {
    if (!(a[0].f > 0.0) || !std::isfinite(a[0].f)) throw dslvm::Error("System.andersen: temperature must be positive");
    if (!(a[1].f > 0.0) || !std::isfinite(a[1].f))
      throw dslvm::Error("System.andersen: collision frequency must be positive (1/ps)");
    S(p).has_andersen = true;
    S(p).andersen_temp = a[0].f;
    S(p).andersen_rate = a[1].f;
    S(p).andersen_seed = (int)a[2].i;
    return Value::object("System", p);
  });
  method(r, "System", "sim", {"string", "float", "float", "float", "int"}, "Sim", [&h](void *p, Args &a) {
    auto sim = build_sim(S(p), h.platform_name, h.threads, a[0].s, a[1].f, a[2].f, a[3].f, a[4].i);
    if (h.sims.size() >= 100000) throw dslvm::Error("Sim object limit exceeded");
    h.sims.push_back(std::move(sim));
    return Value::object("Sim", h.sims.back().get());
  });

  // ---------------------------------------------------------------- Sim
  method(r, "Sim", "velocities", {"float", "int"}, "Sim", [](void *p, Args &a) {
    if (!(a[0].f >= 0.0) || !std::isfinite(a[0].f))
      throw dslvm::Error("Sim.velocities: temperature must be non-negative");
    SIM(p).ctx->setVelocitiesToTemperature(a[0].f, safe_seed(a[1].i));
    return Value::object("Sim", p);
  });
  method(r, "Sim", "minimize", {"float", "int"}, "Sim", [](void *p, Args &a) {
    if (!(a[0].f >= 0.0) || !std::isfinite(a[0].f) || a[1].i < 0)
      throw dslvm::Error("Sim.minimize: tolerance and max iterations must be non-negative");
    if (a[1].i > 100000000) throw dslvm::Error("Sim.minimize: max iterations must be at most 100000000");
    // L-BFGS takes its work vectors (history m = 6: about 18 vectors of 3N doubles) with malloc.
    membudget::require(std::size_t(18) * 3 * SIM(p).spec.particles.size() * sizeof(double));
    OpenMM::LocalEnergyMinimizer::minimize(*SIM(p).ctx, a[0].f, (int)a[1].i);
    SIM(p).check_finite("Sim.minimize");
    return Value::object("Sim", p);
  });
  method(r, "Sim", "report", {"string", "int"}, "Sim", [](void *p, Args &a) {
    if (a[0].s.empty()) throw dslvm::Error("Sim.report: path is empty");
    if (a[1].i < 1) throw dslvm::Error("Sim.report: interval 'every' must be >= 1");
    SimObj &sim = SIM(p);
    if (sim.report_stream.is_open()) sim.report_stream.close();
    sim.report_path = a[0].s;
    sim.report_every = a[1].i;
    sim.report_started = false;
    return Value::object("Sim", p);
  });
  method(r, "Sim", "xyz", {"string", "int"}, "Sim", [](void *p, Args &a) {
    if (a[0].s.empty()) throw dslvm::Error("Sim.xyz: path is empty");
    if (a[1].i < 1) throw dslvm::Error("Sim.xyz: interval 'every' must be >= 1");
    SimObj &sim = SIM(p);
    if (sim.xyz_stream.is_open()) sim.xyz_stream.close();
    sim.xyz_path = a[0].s;
    sim.xyz_every = a[1].i;
    sim.xyz_started = false;
    return Value::object("Sim", p);
  });
  method(r, "Sim", "save_xyz", {"string"}, "Sim", [](void *p, Args &a) {
    if (a[0].s.empty()) throw dslvm::Error("Sim.save_xyz: path is empty");
    std::ofstream f(a[0].s);
    if (!f) throw dslvm::Error("Sim.save_xyz: cannot open " + a[0].s);
    SIM(p).write_xyz_frame(f, a[0].s);
    return Value::object("Sim", p);
  });
  method(r, "Sim", "step", {"int"}, "Sim", [](void *p, Args &a) {
    int64_t n = a[0].i;
    if (n < 0 || n > 10000000) throw dslvm::Error("Sim.step: step count must be in 0..10000000");
    SIM(p).run_steps(n);
    return Value::object("Sim", p);
  });
  method(r, "Sim", "save_pdb", {"string"}, "Sim", [](void *p, Args &a) {
    if (a[0].s.empty()) throw dslvm::Error("Sim.save_pdb: path is empty");
    std::ofstream f(a[0].s);
    if (!f) throw dslvm::Error("Sim.save_pdb: cannot open " + a[0].s);
    SIM(p).write_pdb(f, a[0].s);
    return Value::object("Sim", p);
  });
  method(r, "Sim", "dof", {}, "int", [](void *p, Args &) {
    return Value::integer(SIM(p).dof);
  });
  method(r, "Sim", "mark", {}, "Sim", [](void *p, Args &) {
    SIM(p).mark_pos = SIM(p).ctx->getState(OpenMM::State::Positions).getPositions();
    return Value::object("Sim", p);
  });
  // Mean squared displacement of the mobile particles since Sim.mark() (positions are never wrapped,
  // so periodic systems give the Einstein-relation MSD directly).
  method(r, "Sim", "msd", {}, "float", [](void *p, Args &) {
    SimObj &sim = SIM(p);
    if (sim.mark_pos.empty()) throw dslvm::Error("Sim.msd: call Sim.mark() first to set the reference positions");
    const auto pos = sim.ctx->getState(OpenMM::State::Positions).getPositions();
    double sum = 0.0;
    int count = 0;
    for (size_t i = 0; i < pos.size(); i++) {
      if (sim.spec.particles[i].mass <= 0.0) continue;
      OpenMM::Vec3 d = pos[i] - sim.mark_pos[i];
      sum += d.dot(d);
      count++;
    }
    if (count == 0) throw dslvm::Error("Sim.msd: no mobile particles");
    double v = sum / (double)count;
    if (!std::isfinite(v)) throw dslvm::Error("Sim.msd: displacement is not finite");
    return Value::real(v);
  });
  // Radial distribution function g(r) of a periodic system, averaged over `frames` snapshots taken
  // `stride` steps apart (the current configuration is the first), written as CSV "r g".
  // rdf_pair restricts the pairs to two element symbols ("*" matches every particle).
  auto rdf_impl = [](void *p, const std::string &path, const std::string &sym_a, const std::string &sym_b,
                     double rmax, int64_t bins, int64_t frames, int64_t stride, const char *who) {
    SimObj &sim = SIM(p);
    if (path.empty()) throw dslvm::Error(std::string(who) + ": path is empty");
    if (!sim.spec.has_box) throw dslvm::Error(std::string(who) + ": requires a periodic box");
    if (!(rmax > 0.0) || !std::isfinite(rmax)) throw dslvm::Error(std::string(who) + ": rmax must be positive");
    if (bins < 1 || bins > 100000) throw dslvm::Error(std::string(who) + ": bins must be in 1..100000");
    if (frames < 1 || frames > 1000000) throw dslvm::Error(std::string(who) + ": frames must be in 1..1000000");
    if (stride < 0 || stride > 10000000) throw dslvm::Error(std::string(who) + ": stride must be in 0..10000000");
    size_t n = sim.spec.particles.size();
    if (n > 20000) throw dslvm::Error(std::string(who) + ": at most 20000 particles (pair loop)");
    std::vector<size_t> set_a, set_b;
    for (size_t i = 0; i < n; i++) {
      const std::string &s = sim.spec.particles[i].symbol;
      if (sym_a == "*" || s == sym_a) set_a.push_back(i);
      if (sym_b == "*" || s == sym_b) set_b.push_back(i);
    }
    bool same = sym_a == sym_b;
    if (set_a.empty() || set_b.empty())
      throw dslvm::Error(std::string(who) + ": no particles with symbol '" + (set_a.empty() ? sym_a : sym_b) + "'");
    if (same && set_a.size() < 2) throw dslvm::Error(std::string(who) + ": needs at least two particles");
    std::vector<double> hist((size_t)bins, 0.0);
    double vol_sum = 0.0;
    for (int64_t f = 0; f < frames; f++) {
      if (f > 0 && stride > 0) sim.run_steps(stride);
      OpenMM::State st = sim.ctx->getState(OpenMM::State::Positions);
      OpenMM::Vec3 ba, bb, bc;
      st.getPeriodicBoxVectors(ba, bb, bc);
      double L[3] = {ba[0], bb[1], bc[2]};
      if (rmax >= 0.5 * std::min({L[0], L[1], L[2]}))
        throw dslvm::Error(std::string(who) + ": rmax (" + fmt(rmax) + ") must be less than half the box size (" +
                           fmt(0.5 * std::min({L[0], L[1], L[2]})) + ")");
      vol_sum += L[0] * L[1] * L[2];
      const auto &pos = st.getPositions();
      for (size_t ia = 0; ia < set_a.size(); ia++) {
        size_t i = set_a[ia];
        for (size_t ib = same ? ia + 1 : 0; ib < set_b.size(); ib++) {
          size_t j = set_b[ib];
          if (i == j) continue;
          double d2 = 0.0;
          for (int k = 0; k < 3; k++) {
            double d = pos[j][k] - pos[i][k];
            d -= std::round(d / L[k]) * L[k];
            d2 += d * d;
          }
          double d = std::sqrt(d2);
          if (d < rmax) hist[(size_t)(d / rmax * (double)bins)] += 1.0;
        }
      }
    }
    std::ofstream out(path);
    if (!out) throw dslvm::Error(std::string(who) + ": cannot open " + path);
    out << "# r_nm g_r (" << frames << " frames, " << set_a.size() << " " << sym_a << " x " << set_b.size() << " "
        << sym_b << ")\n";
    double vol = vol_sum / (double)frames;
    // Ideal-gas pair count per shell: N_a N_b / V (each unordered pair once when both sets coincide).
    double pair_density = same ? 0.5 * (double)set_a.size() * (double)set_a.size() / vol
                               : (double)set_a.size() * (double)set_b.size() / vol;
    double dr = rmax / (double)bins;
    for (int64_t b = 0; b < bins; b++) {
      double r1 = (double)b * dr, r2 = r1 + dr;
      double shell = 4.0 / 3.0 * kPi * (r2 * r2 * r2 - r1 * r1 * r1);
      double ideal = pair_density * shell * (double)frames;
      out << fmt(0.5 * (r1 + r2)) << " " << fmt(hist[(size_t)b] / ideal) << "\n";
    }
    out.flush();
    if (!out) throw dslvm::Error(std::string(who) + ": failed writing to " + path);
    return Value::object("Sim", p);
  };
  method(r, "Sim", "rdf", {"string", "float", "int", "int", "int"}, "Sim", [rdf_impl](void *p, Args &a) {
    return rdf_impl(p, a[0].s, "*", "*", a[1].f, a[2].i, a[3].i, a[4].i, "Sim.rdf");
  });
  method(r, "Sim", "rdf_pair", {"string", "string", "string", "float", "int", "int", "int"}, "Sim",
         [rdf_impl](void *p, Args &a) {
           return rdf_impl(p, a[0].s, trim_str(a[1].s), trim_str(a[2].s), a[3].f, a[4].i, a[5].i, a[6].i, "Sim.rdf_pair");
         });
  method(r, "Sim", "ke", {}, "float", [](void *p, Args &) {
    return Value::real(SIM(p).energy_state("Sim.ke").getKineticEnergy());
  });
  method(r, "Sim", "pe", {}, "float", [](void *p, Args &) {
    return Value::real(SIM(p).energy_state("Sim.pe").getPotentialEnergy());
  });
  method(r, "Sim", "energy", {}, "float", [](void *p, Args &) {
    OpenMM::State st = SIM(p).energy_state("Sim.energy");
    return Value::real(st.getKineticEnergy() + st.getPotentialEnergy());
  });
  method(r, "Sim", "temperature", {}, "float", [](void *p, Args &) {
    double ke = SIM(p).energy_state("Sim.temperature").getKineticEnergy();
    return Value::real(SIM(p).compute_temp(ke));
  });
  method(r, "Sim", "time", {}, "float", [](void *p, Args &) {
    return Value::real(SIM(p).ctx->getState(0).getTime());
  });
  method(r, "Sim", "x", {"int"}, "float", [](void *p, Args &a) {
    need_sim_particle(SIM(p), a[0].i, "Sim.x");
    return Value::real(SIM(p).ctx->getState(OpenMM::State::Positions).getPositions()[a[0].i][0]);
  });
  method(r, "Sim", "y", {"int"}, "float", [](void *p, Args &a) {
    need_sim_particle(SIM(p), a[0].i, "Sim.y");
    return Value::real(SIM(p).ctx->getState(OpenMM::State::Positions).getPositions()[a[0].i][1]);
  });
  method(r, "Sim", "z", {"int"}, "float", [](void *p, Args &a) {
    need_sim_particle(SIM(p), a[0].i, "Sim.z");
    return Value::real(SIM(p).ctx->getState(OpenMM::State::Positions).getPositions()[a[0].i][2]);
  });
  method(r, "Sim", "vx", {"int"}, "float", [](void *p, Args &a) {
    need_sim_particle(SIM(p), a[0].i, "Sim.vx");
    return Value::real(SIM(p).ctx->getState(OpenMM::State::Velocities).getVelocities()[a[0].i][0]);
  });
  method(r, "Sim", "vy", {"int"}, "float", [](void *p, Args &a) {
    need_sim_particle(SIM(p), a[0].i, "Sim.vy");
    return Value::real(SIM(p).ctx->getState(OpenMM::State::Velocities).getVelocities()[a[0].i][1]);
  });
  method(r, "Sim", "vz", {"int"}, "float", [](void *p, Args &a) {
    need_sim_particle(SIM(p), a[0].i, "Sim.vz");
    return Value::real(SIM(p).ctx->getState(OpenMM::State::Velocities).getVelocities()[a[0].i][2]);
  });
  method(r, "Sim", "fx", {"int"}, "float", [](void *p, Args &a) {
    need_sim_particle(SIM(p), a[0].i, "Sim.fx");
    return Value::real(SIM(p).force(a[0].i, 0, "Sim.fx"));
  });
  method(r, "Sim", "fy", {"int"}, "float", [](void *p, Args &a) {
    need_sim_particle(SIM(p), a[0].i, "Sim.fy");
    return Value::real(SIM(p).force(a[0].i, 1, "Sim.fy"));
  });
  method(r, "Sim", "fz", {"int"}, "float", [](void *p, Args &a) {
    need_sim_particle(SIM(p), a[0].i, "Sim.fz");
    return Value::real(SIM(p).force(a[0].i, 2, "Sim.fz"));
  });
  method(r, "Sim", "distance", {"int", "int"}, "float", [](void *p, Args &a) {
    SimObj &sim = SIM(p);
    need_sim_particle(sim, a[0].i, "Sim.distance");
    need_sim_particle(sim, a[1].i, "Sim.distance");
    OpenMM::State st = sim.ctx->getState(OpenMM::State::Positions);
    const auto &pos = st.getPositions();
    double dx = pos[a[1].i][0] - pos[a[0].i][0];
    double dy = pos[a[1].i][1] - pos[a[0].i][1];
    double dz = pos[a[1].i][2] - pos[a[0].i][2];
    if (sim.spec.has_box) {
      OpenMM::Vec3 ba, bb, bc;
      st.getPeriodicBoxVectors(ba, bb, bc);
      dx -= std::round(dx / ba[0]) * ba[0];
      dy -= std::round(dy / bb[1]) * bb[1];
      dz -= std::round(dz / bc[2]) * bc[2];
    }
    return Value::real(std::sqrt(dx * dx + dy * dy + dz * dz));
  });
  method(r, "Sim", "angle_val", {"int", "int", "int"}, "float", [](void *p, Args &a) {
    SimObj &sim = SIM(p);
    need_sim_particle(sim, a[0].i, "Sim.angle_val");
    need_sim_particle(sim, a[1].i, "Sim.angle_val");
    need_sim_particle(sim, a[2].i, "Sim.angle_val");
    OpenMM::State st = sim.ctx->getState(OpenMM::State::Positions);
    const auto &pos = st.getPositions();
    OpenMM::Vec3 u = pos[a[0].i] - pos[a[1].i];
    OpenMM::Vec3 v = pos[a[2].i] - pos[a[1].i];
    double nu = std::sqrt(u.dot(u));
    double nv = std::sqrt(v.dot(v));
    if (nu == 0.0 || nv == 0.0) throw dslvm::Error("Sim.angle_val: degenerate zero-length bond vector");
    double c = std::clamp(u.dot(v) / (nu * nv), -1.0, 1.0);
    return Value::real(std::acos(c));
  });
  method(r, "Sim", "box_volume", {}, "float", [](void *p, Args &) {
    OpenMM::State st = SIM(p).ctx->getState(0);
    OpenMM::Vec3 a, b, c;
    st.getPeriodicBoxVectors(a, b, c);
    return Value::real(a[0] * b[1] * c[2]);
  });
}

const char *usage =
    "usage: lmmolecule --version | --notices | --dsl-schema [--json] | --dsl-signatures | [--dry-run] --dsl FILE [FILE ...]\n"
    "environment: LMMOLECULE_MAX_MEMORY_MB  total memory budget in MiB (default 4096); a program that needs more\n"
    "             stops with a run error (exit 3) instead of allocating\n";

// The CPU platform computes forces on OpenMM's worker thread (ThreadPool, even with Threads=1), whose
// thread body has no handler: the budget running out there (say, while the neighbour list grows) ends
// in std::terminate. Report that like any run error (JSON line, exit 3) instead of aborting.
std::terminate_handler previous_terminate = nullptr;
[[noreturn]] void on_terminate() {
  if (std::exception_ptr ep = std::current_exception()) {
    try {
      std::rethrow_exception(ep);
    } catch (const membudget::Exceeded &e) {
      dslvm::report_error("run", dslvm::Error(e.what()));
      std::cout.flush();
      std::cerr.flush();
      std::_Exit(3);
    } catch (...) {
    }
  }
  if (previous_terminate) previous_terminate();
  std::abort();
}
}  // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
#endif
  registerCpuPlatform();
  Host host;
  dslvm::Registry reg;
  register_host(reg, host);

  std::string a1 = argc > 1 ? argv[1] : "";
  if (argc == 2 && a1 == "--version") {
    std::cout << "lmmolecule " LMMOLECULE_VERSION "\n";
    return std::cout ? 0 : 4;
  }
  if (argc == 2 && a1 == "--notices") {
    std::cout.write(reinterpret_cast<const char *>(lmmolecule::embedded_notices),
                    (std::streamsize)lmmolecule::embedded_notices_size);
    return std::cout ? 0 : 4;
  }
  if (argc >= 2 && a1 == "--dsl-schema") {
    if (argc == 2) {
      for (const auto &s : reg.signatures()) std::cout << s << "\n";
      return std::cout ? 0 : 4;
    }
    if (argc == 3 && std::string(argv[2]) == "--json") {
      std::cout.write(reinterpret_cast<const char *>(lmmolecule::embedded_schema),
                      (std::streamsize)lmmolecule::embedded_schema_size);
      return std::cout ? 0 : 4;
    }
    std::cerr << usage;
    return 2;
  }
  if (argc == 2 && a1 == "--dsl-signatures") {
    for (const auto &s : reg.signatures()) std::cout << s << "\n";
    return 0;
  }

  bool dry = false;
  int dsl_arg = 1;
  if (argc > 1 && std::string(argv[dsl_arg]) == "--dry-run") {
    dry = true;
    ++dsl_arg;
  }
  if (argc <= dsl_arg || std::string(argv[dsl_arg]) != "--dsl") {
    std::cerr << usage;
    return 2;
  }
  std::vector<std::string> files;
  for (int i = dsl_arg + 1; i < argc; i++) {
    if (std::string(argv[i]) == "--dry-run") {
      if (dry) {
        std::cerr << usage;
        return 2;
      }
      dry = true;
    } else if (std::string(argv[i]).empty() || std::string(argv[i]).rfind("--", 0) == 0) {
      std::cerr << usage;
      return 2;
    } else {
      files.push_back(argv[i]);
    }
  }
  if (files.empty()) {
    std::cerr << usage;
    return 2;
  }

  if (!membudget::configure("lmmolecule", "LMMOLECULE_MAX_MEMORY_MB")) {
    std::cerr << usage;
    return 2;
  }
  previous_terminate = std::set_terminate(on_terminate);
  const std::string schema(reinterpret_cast<const char *>(lmmolecule::embedded_schema),
                           lmmolecule::embedded_schema_size);
  dslvm::Context context;
  context.set("md", &host);
  std::vector<dslvm::Program> programs;
  for (const auto &file : files) {
    try {
      programs.push_back(dslvm::Program::load(dslvm::read_file(file), schema, reg));
    } catch (const dslvm::Error &e) {
      return dslvm::report_error("load", e);
    } catch (const std::exception &e) {
      return dslvm::report_error("load", dslvm::Error(e.what()));
    }
  }
  if (dry) {
    dslvm::report_ok();
    return 0;
  }
  for (const auto &p : programs) {
    try {
      p.run(context, reg);
    } catch (const dslvm::Error &e) {
      return dslvm::report_error("run", e);
    } catch (const std::exception &e) {
      return dslvm::report_error("run", dslvm::Error(e.what()));  // budget in the VM
    }
    dslvm::report_ok();
  }
  return 0;
}
