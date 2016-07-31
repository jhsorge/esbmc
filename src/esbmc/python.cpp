#include "parseoptions.h"
#include <irep2.h>
#include <migrate.h>
#include <solvers/smt/smt_conv.h>
#include <langapi/mode.h>
#include <goto-programs/goto_functions.h>

#include <boost/python.hpp>
#include <boost/python/object/find_instance.hpp>
using namespace boost::python;

void dereference_handlers_init(void);
void build_bigint_python_class();
void build_base_expr2t_python_class();
void build_base_type2t_python_class();
void build_type2t_container_converters();
void build_expr2t_container_converters();
void build_dstring_python_class();
void build_smt_conv_python_class();
void build_goto_func_class();
void build_fixedbv_python_class();

class dummy_expr_class { };
class dummy_type_class { };

// Prevent more than one instance per process
static bool python_module_engaged = false;
// Parseoptions instance representing an esbmc process
static cbmc_parseoptionst *po = NULL;
static namespacet *ns = NULL;
// Type pool needs to live as long as the process.
static type_poolt *tp = NULL;

dict type_to_downcast;
dict expr_to_downcast;

template <typename T>
class migrate_func;

template<>
class migrate_func<type2tc>
{
public:
  static void converter(const typet *type, type2tc *out) { migrate_type(*type, *out); }
};

template<>
class migrate_func<expr2tc>
{
public:
  static void converter(const exprt *expr, expr2tc *out) { migrate_expr(*expr, *out); }
};

template<>
class migrate_func<typet>
{
public:
  static void converter(const type2tc *type, typet *out) { *out = migrate_type_back(*type); }
};

template<>
class migrate_func<exprt>
{
public:
  static void converter(const expr2tc *expr, exprt *out) { *out = migrate_expr_back(*expr); }
};

template <typename From, typename To>
struct oldirep_to_newirep
{
public:
    oldirep_to_newirep()
    {
      // Store a (global) converter pointer
      using namespace boost::python;
      // Rvalue converter
      converter::registry::insert(&convertible, &cons, type_id<To>(),
                          &converter::expected_from_python_type_direct<To>::get_pytype);
    }

 private:
    static void* convertible(PyObject* p)
    {
      using namespace boost::python;

      // For this set of converters, refuse to produce nil ireps. There are
      // other (similar) converters for that.
      if (p == Py_None)
          return NULL;

      objects::instance<> *inst =
        reinterpret_cast<objects::instance<>*>(p);
      (void)inst; // For debug / inspection

      // Scatter consts around to ensure that the get() below doesn't trigger
      // detachment.
      const From *foo =
        reinterpret_cast<From*>(
            objects::find_instance_impl(p, boost::python::type_id<From>()));

      // Find object instance may fail
      if (!foo)
        return NULL;

      // Can't actually create an lvalue because there's no storage, but we
      // can convert to an rvalue. This function will be called to work out
      // if it can be converted, so return non-null to indicate that.
      return const_cast<void*>(reinterpret_cast<const void*>(foo));
    }

    static void cons(PyObject *src __attribute__((unused)), boost::python::converter::rvalue_from_python_stage1_data *stage1)
    {
      using namespace boost::python;
      converter::rvalue_from_python_data<To> *store =
        reinterpret_cast<converter::rvalue_from_python_data<To>*>(stage1);

      To *obj_store = reinterpret_cast<To *>(&store->storage.bytes);

      // Create an rvalue from the ptr stored by convertible.
      const From *oldptr = reinterpret_cast<const From *>(stage1->convertible);

      // Construct container so it isn't uninitialized memory...
      new (obj_store) To();

      (*migrate_func<To>::converter)(oldptr, obj_store);

      // Let rvalue holder know that needs deconstructing please
      store->stage1.convertible = obj_store;

      // fini
      return;
    }
};

static boost::python::object
init_esbmc_process(boost::python::object o)
{
  using namespace boost::python;
  std::vector<std::string> str_list;
  const char **argv;
  unsigned int argc, i;
  int result;

  // Arguments: list of options that we would otherwise provide on the ESBMC
  // command line. Convert these to argc/argv for parseoptions, create
  // parseoptions object, perform initial processing, and return handles to
  // useful structures.

  // Extract list from object; provoke exception if needs be.
  list l = extract<list>(o);

  // Apparently no good way to iterate over list
  while (len(l) > 0) {
    object s = l.pop();
    str_list.push_back(extract<std::string>(s));
  }

  // Convert the list of C++ lists to C argc / argv.
  argc = str_list.size();
  argc += 2; // Extra options we add.
  argv = (const char**)malloc(sizeof(const char *) * argc);
  i = 0;
  argv[i++] = "esbmc"; // ESBMC expects program path to be first arg
  for (const std::string &s : str_list)
    argv[i++] = s.data();

  // Add skip-bmc option: causes all usual processing to happen, but we bail
  // out of parseoptions at the point where we would usually start BMC.
  argv[i++] = "--skip-bmc";

  // Init esbmc Stuff. First the static order initialization fiasco.
  tp = new type_poolt(true);
  type_pool = *tp;
  init_expr_constants();
  dereference_handlers_init();

  python_module_engaged = true;
  po = new cbmc_parseoptionst(argc, argv);
  free(argv);

  // Perform initial processing
  result = po->doit();

  // Assuming we didn't abort; if there's an error, return None. Otherwise
  // construct a tuple of useful handles.
  if (result != 0) {
    delete po;
    delete tp;
    python_module_engaged = false;
    return object();
  }

  ns = new namespacet(po->context);

  // Convert return values to python objects (TM). Wrap into a PyObject, stuff
  // in handle, transfer to object. Unclear if there's a supported way of
  // doing this with opaque pointers in the API: I get the impression that
  // they're only supposed to be single return values.
  auto converter1 = converter::registry::lookup(type_id<namespacet*>());
  handle<> nsh(converter1.to_python(&ns));
  object nso(nsh);
  // Config options are global. Woo.
  auto converter2 = converter::registry::lookup(type_id<optionst*>());
  auto opt_ptr = &config.options;
  handle<> optsh(converter2.to_python(&opt_ptr));
  object opts(optsh);
  // Goto functions handle comes from inside the parseoptions object. Pass it
  // through the ref-existing-ptr process, as we need to access it's internals.
  // Don't want to copy by value as it can be massive.
  reference_existing_object::apply<goto_functionst*>::type gf_cvt;
  PyObject *gfp = gf_cvt(&po->goto_functions);
  handle<> funch(gfp);
  object funcs(funch);

  return make_tuple(nso, opts, funcs);
}

static void
kill_esbmc_process(void)
{
  if (!python_module_engaged)
    // Nope
    return;

  assert(po != NULL && ns != NULL && tp != NULL);

  // It's the users problem if they haven't actually cleaned up their python
  // references.
  delete ns;
  ns = NULL;
  delete po;
  po = NULL;
  delete tp;
  tp = NULL;
  python_module_engaged = false;

  return;
}

// For numerous reasons we want facilities to downcast a type2tc or expr2tc
// to the corresponding something2tc class, allowing python to access the
// contents. However, to_##thetype##_type etc are a) overloaded, and b) don't
// return something2tc's. And we can't register the something2tc constructor
// as a simple function. So we get this:
template <typename Result, typename Source>
Result
downcast_vehicle(const Source &contained)
{
  // Just construct a new container around this.
  return Result(contained);
}

object
downcast_type(const type2tc &type)
{
  assert(type->type_id < type2t::end_type_id);
  object o = type_to_downcast[type->type_id];
  return o(type);
}

object
downcast_expr(const expr2tc &expr)
{
  assert(expr->expr_id < expr2t::end_expr_id);
  object o = expr_to_downcast[expr->expr_id];
  return o(expr);
}

BOOST_PYTHON_MODULE(esbmc)
{
  // This is essentially the entry point for the esbmc shared object.
  // Workarounds for the static order initialization are in init_esbmc_process
  // due to some annoyance with object lifetime.

  // Register process init and sort-of deconstruction.
  def("init_esbmc_process", &init_esbmc_process);
  def("kill_esbmc_process", &kill_esbmc_process);

  // Use boost preprocessing iteration to enumerate all irep classes and
  // register them into python. In the future this should be done via types
  // so that it can actually be typechecked, but that will require:
  //  * A boost set of ireps to exist
  //  * expr_id's to be registered into a template like irep_methods2.

  // Namespace into types and exprs.
  {
    auto types = class_<dummy_type_class>("type");
    scope quux = types;

    build_base_type2t_python_class();

    types.def("is_nil_type", &is_nil_type);
    types.staticmethod("is_nil_type");

    // In this scope, define the old irep types to
    class_<typet>("typet", no_init);
    class_<code_typet, bases<typet> >("code_typet", no_init);

#define _ESBMC_IREP2_MPL_TYPE_SET(r, data, elem) BOOST_PP_CAT(elem,_type2t)::build_python_class(type2t::BOOST_PP_CAT(elem,_id));
BOOST_PP_LIST_FOR_EACH(_ESBMC_IREP2_MPL_TYPE_SET, foo, ESBMC_LIST_OF_TYPES)

    build_type2t_container_converters();

    // Build downcasting infrastructure
    type_to_downcast = dict();
#define _ESBMC_IREP2_TYPE_DOWNCASTING(r, data, elem) \
    type_to_downcast[type2t::BOOST_PP_CAT(elem,_id)] = \
        make_function(downcast_vehicle<BOOST_PP_CAT(elem,_type2tc), type2tc>);
BOOST_PP_LIST_FOR_EACH(_ESBMC_IREP2_TYPE_DOWNCASTING, foo, ESBMC_LIST_OF_TYPES)
  }

  {
    auto exprs = class_<dummy_expr_class>("expr");
    scope quux = exprs;

    exprs.def("is_nil_expr", &is_nil_expr);
    exprs.staticmethod("is_nil_expr");

    // Define old expr class too
    class_<exprt>("exprt", no_init);

    build_base_expr2t_python_class();
#define _ESBMC_EXPR2_MPL_EXPR_SET(r, data, elem) BOOST_PP_CAT(elem,2t)::build_python_class(expr2t::BOOST_PP_CAT(elem,_id));
BOOST_PP_LIST_FOR_EACH(_ESBMC_EXPR2_MPL_EXPR_SET, foo, ESBMC_LIST_OF_EXPRS)

    build_expr2t_container_converters();

    // Build downcasting infrastructure
    expr_to_downcast = dict();
#define _ESBMC_IREP2_EXPR_DOWNCASTING(r, data, elem) \
    expr_to_downcast[expr2t::BOOST_PP_CAT(elem,_id)] = \
        make_function(downcast_vehicle<BOOST_PP_CAT(elem,2tc), expr2tc>);
BOOST_PP_LIST_FOR_EACH(_ESBMC_IREP2_EXPR_DOWNCASTING, foo, ESBMC_LIST_OF_EXPRS)

  }

  // Register BigInt globally
  build_bigint_python_class();
  build_dstring_python_class();

  // Alas, we need to pass handles to optionst, namespace, goto funcs around.
  // User should be able to extract them from whatever execution context they
  // generate.
  opaque<optionst>();
  opaque<namespacet>();

  // Build smt solver related stuff
  build_smt_conv_python_class();

  // Build goto function class representions.
  build_goto_func_class();

  // Build fixedbvt class
  build_fixedbv_python_class();

  // Register old-irep to new-irep converters
  oldirep_to_newirep<typet, type2tc>();
  oldirep_to_newirep<exprt, expr2tc>();

  // And backwards
  oldirep_to_newirep<type2tc, typet>();
  oldirep_to_newirep<expr2tc, exprt>();

  def("downcast_type", &downcast_type);
  def("downcast_expr", &downcast_expr);
}

// Include these other things that are special to the esbmc binary:

const mode_table_et mode_table[] =
{
  LANGAPI_HAVE_MODE_C,
#ifndef WITHOUT_CLANG
  LANGAPI_HAVE_MODE_CLANG_C,
#endif
  LANGAPI_HAVE_MODE_CPP,
#ifndef WITHOUT_CLANG
  LANGAPI_HAVE_MODE_CLANG_CPP,
#endif
  LANGAPI_HAVE_MODE_END
};

extern "C" uint8_t buildidstring_buf[1];
uint8_t *version_string = buildidstring_buf;