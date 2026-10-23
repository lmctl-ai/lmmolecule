// lmmolecule: OpenMM 8 (Core API, Reference and CPU platforms, Lepton, PocketFFT, L-BFGS)
// behind an lmscript `_dsl_` schema. Scripting only: the executable has no user-facing CLI
// besides --dsl and the standard schema/version/notices queries.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
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
#include "lepton/ExpressionTreeNode.h"
#include "lepton/Operation.h"
#include "lepton/ParsedExpression.h"
#include "lepton/Parser.h"

#include "dslvm.hpp"
#include "notices_embedded.hpp"
#include "schema_embedded.hpp"

extern "C" void registerCpuPlatform();

namespace {
using dslvm::Args;
using dslvm::Value;

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

struct SystemSpec {
  bool has_box = false;
  double box_a = 0.0, box_b = 0.0, box_c = 0.0;
  std::vector<ParticleSpec> particles;
  std::vector<BondSpec> bonds;
  std::vector<AngleSpec> angles;
  std::vector<TorsionSpec> torsions;
  bool has_lj = false;
  double default_lj_eps = 0.0;
  double default_lj_sigma = 0.1;
  double lj_cutoff = 0.0;
  bool has_charges = false;
  bool has_coulomb = false;
  std::string coulomb_method = "none";
  double coulomb_cutoff = 0.0;
  std::set<std::pair<int, int>> exclusions;
  std::vector<CustomExternalSpec> custom_externals;
  std::vector<CustomBondSpec> custom_bonds;
  std::vector<CustomNonbondedSpec> custom_nonbondeds;
  bool has_barostat = false;
  double baro_pressure = 1.0;
  double baro_temp = 300.0;
  int baro_freq = 25;
  int baro_seed = 1;
};

struct SimObj {
  SystemSpec spec;
  std::unique_ptr<OpenMM::System> sys;
  std::unique_ptr<OpenMM::Integrator> integ;
  std::unique_ptr<OpenMM::Context> ctx;
  int mobile_particles = 0;
  int64_t total_steps = 0;

  std::string report_path;
  int64_t report_every = 0;
  bool report_started = false;
  std::ofstream report_stream;

  std::string xyz_path;
  int64_t xyz_every = 0;
  bool xyz_started = false;
  std::ofstream xyz_stream;

  double compute_temp(double ke) const {
    if (mobile_particles <= 0) return 0.0;
    return (2.0 * ke) / (3.0 * (double)mobile_particles * BOLTZ);
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

double uniform01(std::mt19937_64 &rng) {
  return (double)(rng() >> 11) * (1.0 / 9007199254740992.0);
}

int safe_seed(int64_t s) {
  int v = (int)(s & 0x7fffffff);
  return v == 0 ? 1 : v;
}

std::unique_ptr<SimObj> build_sim(const SystemSpec &spec, const std::string &platform_name,
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
    nb->setEwaldErrorTolerance(1e-5);
    nb->setUseDispersionCorrection(false);

    for (const auto &p : spec.particles) {
      double eps = p.has_lj ? p.lj_eps : (spec.has_lj ? spec.default_lj_eps : 0.0);
      double sig = p.has_lj ? p.lj_sigma : (spec.has_lj ? spec.default_lj_sigma : 0.1);
      nb->addParticle(p.charge, sig, eps);
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
    props["Threads"] = "1";
    props["DeterministicForces"] = "true";
  }
  sim->ctx = std::make_unique<OpenMM::Context>(*sim->sys, *sim->integ, plat, props);
  sim->ctx->setPositions(init_pos);
  sim->ctx->setVelocities(init_vel);
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
    S(p).has_box = true;
    S(p).box_a = box;
    S(p).box_b = box;
    S(p).box_c = box;
    std::mt19937_64 rng((uint64_t)seed);
    double min_dist = 0.35 * box / std::cbrt((double)n);
    double min_dist2 = min_dist * min_dist;
    size_t start_idx = S(p).particles.size();
    for (int64_t i = 0; i < n; i++) {
      OpenMM::Vec3 cand;
      for (int attempt = 0; attempt < 200; attempt++) {
        cand = OpenMM::Vec3(uniform01(rng) * box, uniform01(rng) * box, uniform01(rng) * box);
        bool ok = true;
        for (size_t j = start_idx; j < S(p).particles.size(); j++) {
          double dx = cand[0] - S(p).particles[j].pos[0];
          double dy = cand[1] - S(p).particles[j].pos[1];
          double dz = cand[2] - S(p).particles[j].pos[2];
          dx -= std::round(dx / box) * box;
          dy -= std::round(dy / box) * box;
          dz -= std::round(dz / box) * box;
          if (dx * dx + dy * dy + dz * dz < min_dist2) {
            ok = false;
            break;
          }
        }
        if (ok) break;
      }
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
    if (m != "none" && m != "nocutoff" && m != "cutoff" && m != "ewald" && m != "pme")
      throw dslvm::Error("System.coulomb: unknown method '" + a[0].s +
                         "' (expected 'none', 'cutoff', 'ewald', or 'pme')");
    if (!std::isfinite(a[1].f) || a[1].f < 0.0)
      throw dslvm::Error("System.coulomb: cutoff must be non-negative");
    if ((m == "cutoff" || m == "ewald" || m == "pme") && !(a[1].f > 0.0))
      throw dslvm::Error("System.coulomb: method '" + m + "' requires positive cutoff");
    S(p).has_coulomb = true;
    S(p).coulomb_method = m;
    S(p).coulomb_cutoff = a[1].f;
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
    if (!std::isfinite(a[0].f) || !(a[1].f >= 0.0) || !std::isfinite(a[1].f) || a[2].i < 1)
      throw dslvm::Error("System.barostat: invalid barostat parameters");
    S(p).has_barostat = true;
    S(p).baro_pressure = a[0].f;
    S(p).baro_temp = a[1].f;
    S(p).baro_freq = (int)a[2].i;
    S(p).baro_seed = (int)a[3].i;
    return Value::object("System", p);
  });
  method(r, "System", "sim", {"string", "float", "float", "float", "int"}, "Sim", [&h](void *p, Args &a) {
    auto sim = build_sim(S(p), h.platform_name, a[0].s, a[1].f, a[2].f, a[3].f, a[4].i);
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
    SimObj &sim = SIM(p);
    if (!sim.report_path.empty() && !sim.report_started) {
      sim.report_stream.open(sim.report_path, std::ios::out | std::ios::trunc);
      if (!sim.report_stream) throw dslvm::Error("Sim.report: cannot open " + sim.report_path);
      sim.report_stream << "# step time ke pe energy temperature\n";
      sim.report_started = true;
      sim.write_report_row();
    }
    if (!sim.xyz_path.empty() && !sim.xyz_started) {
      sim.xyz_stream.open(sim.xyz_path, std::ios::out | std::ios::trunc);
      if (!sim.xyz_stream) throw dslvm::Error("Sim.xyz: cannot open " + sim.xyz_path);
      sim.xyz_started = true;
      sim.write_xyz_frame(sim.xyz_stream, sim.xyz_path);
    }
    int64_t rem = n;
    while (rem > 0) {
      int64_t chunk = rem;
      if (sim.report_every > 0) {
        int64_t to_rep = sim.report_every - (sim.total_steps % sim.report_every);
        chunk = std::min(chunk, to_rep);
      }
      if (sim.xyz_every > 0) {
        int64_t to_xyz = sim.xyz_every - (sim.total_steps % sim.xyz_every);
        chunk = std::min(chunk, to_xyz);
      }
      sim.integ->step((int)chunk);
      sim.total_steps += chunk;
      rem -= chunk;
      sim.check_finite("Sim.step");
      if (sim.report_every > 0 && (sim.total_steps % sim.report_every == 0)) sim.write_report_row();
      if (sim.xyz_every > 0 && (sim.total_steps % sim.xyz_every == 0))
        sim.write_xyz_frame(sim.xyz_stream, sim.xyz_path);
    }
    return Value::object("Sim", p);
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
    "usage: lmmolecule --version | --notices | --dsl-schema [--json] | --dsl-signatures | [--dry-run] --dsl FILE [FILE ...]\n";
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
    }
    dslvm::report_ok();
  }
  return 0;
}
