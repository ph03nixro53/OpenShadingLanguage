// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage


#include <cmath>
#include <fstream>
#include <iostream>
#include <locale>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <OpenImageIO/imageio.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/imagebufalgo_util.h>
#include <OpenImageIO/imagecache.h>
#include <OpenImageIO/argparse.h>
#include <OpenImageIO/strutil.h>
#include <OpenImageIO/sysutil.h>
#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/timer.h>

#ifdef OSL_USE_OPTIX
// purely to get optix version -- once optix 7.0 is required this can go away
#include <optix.h>
#endif

#include <OSL/oslexec.h>
#include <OSL/oslcomp.h>
#include <OSL/oslquery.h>
#if OSL_USE_BATCHED
#   include <OSL/batched_shaderglobals.h>
#endif
#include "optixgridrender.h"
#include "simplerend.h"

using namespace OSL;
using OIIO::ParamValue;
using OIIO::ParamValueList;

static ShadingSystem *shadingsys = NULL;
static std::vector<std::string> shadernames;
static std::vector<std::string> outputfiles;
static std::vector<std::string> outputvars;
static std::vector<ustring> outputvarnames;
static std::vector<TypeDesc> outputvartypes;
static std::string dataformatname = "";
static std::vector<std::string> entrylayers;
static std::vector<std::string> entryoutputs;
static std::vector<int> entrylayer_index;
static std::vector<const ShaderSymbol *> entrylayer_symbols;
static bool debug1 = false;
static bool debug2 = false;
static bool llvm_debug = false;
static bool verbose = false;
static bool runstats = false;
static bool batched = false;
static int max_batch_size = -1;
static int batch_size = -1;
static bool vary_Pdxdy = false;
static bool vary_udxdy = false;
static bool vary_vdxdy = false;
static bool saveptx = false;
static bool warmup = false;
static bool profile = false;
static bool O0 = false, O1 = false, O2 = false;
static int llvm_opt = 1; // LLVM optimization level
static bool pixelcenters = false;
static bool debugnan = false;
static bool debug_uninit = false;
static bool use_group_outputs = false;
static bool do_oslquery = false;
static bool inbuffer = false;
static bool use_shade_image = false;
static bool userdata_isconnected = false;
static bool print_outputs = false;
static bool output_placement = true;
static bool use_optix = OIIO::Strutil::stoi(OIIO::Sysutil::getenv("TESTSHADE_OPTIX"));
static int xres = 1, yres = 1;
static int num_threads = 0;
static std::string groupname;
static std::string groupspec;
static std::string layername;
static std::vector<std::string> connections;
static ParamValueList params;
static ParamValueList reparams;
static std::string reparam_layer;
static ErrorHandler errhandler;
static int iters = 1;
static std::string raytype = "camera";
static bool raytype_opt = false;
static std::string extraoptions;
static std::string texoptions;
static OSL::Matrix44 Mshad;  // "shader" space to "common" space matrix
static OSL::Matrix44 Mobj;   // "object" space to "common" space matrix
static ShaderGroupRef shadergroup;
static std::string archivegroup;
static int exprcount = 0;
static bool shadingsys_options_set = false;
static float uscale = 1, vscale = 1;
static float uoffset = 0, voffset = 0;
static std::vector<const char*> shader_setup_args;
static std::string localename = OIIO::Sysutil::getenv("TESTSHADE_LOCALE");
static OIIO::ParamValueList userdata;
static char* userdata_base_ptr = nullptr;
static char* output_base_ptr = nullptr;



static void
inject_params ()
{
    for (auto&& pv : params)
        shadingsys->Parameter (*shadergroup, pv.name(), pv.type(), pv.data(),
                               pv.interp() == ParamValue::INTERP_CONSTANT);
}



// Set shading system global attributes based on command line options.
static void
set_shadingsys_options ()
{
    // If benchmarking it isn't necessary to clear the memory. however for
    // unit tests and tracking down early exit issues we may not want the
    // previous sample's group data masquerading as correct values for the
    // next sample, who due to a bug, may not have correct control flow and
    // not actually write to those values.
    OSL_DEV_ONLY(shadingsys->attribute ("clearmemory", 1));

    // Always generate llvm debugging info
    shadingsys->attribute ("llvm_debugging_symbols", 1);

    // Always emit llvm Intel profiling events
    shadingsys->attribute ("llvm_profiling_events", 1);

    OSL_DEV_ONLY(llvm_debug = true);
    shadingsys->attribute ("llvm_debug", (llvm_debug ? 2 : 0));

    shadingsys->attribute ("debug", debug2 ? 2 : (debug1 ? 1 : 0));
    shadingsys->attribute ("compile_report", debug1|debug2);
    int opt = 2;  // default
    if (O0) opt = 0;
    if (O1) opt = 1;
    if (O2) opt = 2;
    if (const char *opt_env = getenv ("TESTSHADE_OPT"))  // overrides opt
        opt = atoi(opt_env);
    shadingsys->attribute ("optimize", opt);

    // The cost of more optimization passes usually pays for itself by
    // reducing the number of instructions JIT ultimately has to lower to
    // the target ISA.
    if (const char *llvm_opt_env = getenv ("TESTSHADE_LLVM_OPT"))  // overrides llvm_opt
        llvm_opt = atoi(llvm_opt_env);
    shadingsys->attribute ("llvm_optimize", llvm_opt);

    shadingsys->attribute ("profile", int(profile));
    shadingsys->attribute ("lockgeom", 1);
    shadingsys->attribute ("debug_nan", debugnan);
    shadingsys->attribute ("debug_uninit", debug_uninit);
    shadingsys->attribute ("userdata_isconnected", userdata_isconnected);

	// build searchpath for ISA specific OSL shared libraries based on expected
    // location of library directories relative to the executables path.
    // Users can overide using the "options" command line option
    // with "searchpath:library" 
    static const char * relative_lib_dirs[] =
#if (defined(_WIN32) || defined(_WIN64))
        {"\\..\\lib64", "\\..\\lib"};
#else
        {"/../lib64", "/../lib"};
#endif
    auto executable_directory = OIIO::Filesystem::parent_path(OIIO::Sysutil::this_program_path());
    int dirNum = 0;
	std::string librarypath;
    for (const char * relative_lib_dir:relative_lib_dirs) {
        if(dirNum++ > 0) librarypath	+= ":";
        librarypath += executable_directory + relative_lib_dir;
    }
    shadingsys->attribute ("searchpath:library", librarypath);

    if (extraoptions.size())
        shadingsys->attribute ("options", extraoptions);
    if (texoptions.size())
        shadingsys->texturesys()->attribute ("options", texoptions);

    if (const char *opt_env = getenv ("TESTSHADE_BATCHED"))
        batched = atoi(opt_env);

    max_batch_size = 16;
    if (const char *opt_env = getenv ("TESTSHADE_MAX_BATCH_SIZE"))
        max_batch_size = atoi(opt_env);

    batch_size = -1;
    if (const char *opt_env = getenv ("TESTSHADE_BATCH_SIZE"))
        batch_size = atoi(opt_env);

    // For batched allow FMA if build of OSL supports it
    int llvm_jit_fma = batched;
    if (const char *opt_env = getenv ("TESTSHADE_LLVM_JIT_FMA"))
        llvm_jit_fma = atoi(opt_env);
    shadingsys->attribute ("llvm_jit_fma", llvm_jit_fma);

    if (batched) {
#if OSL_USE_BATCHED
        bool batch_size_requested = (batch_size != -1);
        // FIXME: For now, output placement is not supported for batched
        // shading.
        output_placement = false;
        // Not really looping, just emulating goto behavior using break
        for(;;) {
            if (!batch_size_requested || batch_size == 16) {
                if (shadingsys->configure_batch_execution_at(16)) {
                    batch_size = 16;
                    break;
                }
            }
            if (!batch_size_requested || batch_size == 8) {
                if (shadingsys->configure_batch_execution_at(8)) {
                    batch_size = 8;
                    break;
                }
            }
            std::cout << "WARNING:  Hardware or library requirements to utilize batched execution";
            ustring llvm_jit_target;
            shadingsys->getattribute ("llvm_jit_target", llvm_jit_target);
            int llvm_jit_fma;
            shadingsys->getattribute ("llvm_jit_fma", llvm_jit_fma);
            if (!llvm_jit_target.empty()) {
                std::cout << " for isa("<<llvm_jit_target.c_str()<<") and ";
            }
            std::cout << " llvm_jit_fma("<<llvm_jit_fma<<")";
            if (batch_size_requested) {
                std::cout << " and batch_size("<<batch_size<<")";
            }
            std::cout << " are not met, ignoring batched and using single point interface to OSL" << std::endl;
            batched = false;
            break;
        }
#else
        batched = false;
#endif
    }

    if (!batched) {
        // NOTE:  When opt_batched_analysis is enabled,
        // uniform and varying temps will not coalesce
        // with each other.  Neither will symbols
        // with differing forced_llvm_bool() values.
        // This might reduce observed symbol reduction.
        // So disable the analysis when we are not
        // performing batched execution
        shadingsys->attribute ("opt_batched_analysis", 0);
    }

    if (use_optix) {
        // FIXME: For now, output placement is disabled for OptiX mode
        output_placement = false;
    }

    shadingsys_options_set = true;
}



static void
compile_buffer (const std::string &sourcecode,
                const std::string &shadername)
{
    // std::cout << "source was\n---\n" << sourcecode << "---\n\n";
    std::string osobuffer;
    OSLCompiler compiler;
    std::vector<std::string> options;

    if (! compiler.compile_buffer (sourcecode, osobuffer, options)) {
        std::cerr << "Could not compile \"" << shadername << "\"\n";
        exit (EXIT_FAILURE);
    }
    // std::cout << "Compiled to oso:\n---\n" << osobuffer << "---\n\n";

    if (! shadingsys->LoadMemoryCompiledShader (shadername, osobuffer)) {
        std::cerr << "Could not load compiled buffer from \""
                  << shadername << "\"\n";
        exit (EXIT_FAILURE);
    }
}



static void
shader_from_buffers (std::string shadername)
{
    std::string oslfilename = shadername;
    if (! OIIO::Strutil::ends_with (oslfilename, ".osl"))
        oslfilename += ".osl";
    std::string sourcecode;
    if (! OIIO::Filesystem::read_text_file (oslfilename, sourcecode)) {
        std::cerr << "Could not open \"" << oslfilename << "\"\n";
        exit (EXIT_FAILURE);
    }

    compile_buffer (sourcecode, shadername);
    // std::cout << "Read and compiled " << shadername << "\n";
}



static int
add_shader (int argc, const char *argv[])
{
    OSL_DASSERT(argc == 1);
    string_view shadername (argv[0]);

    set_shadingsys_options ();

    if (inbuffer)  // Request to exercise the buffer-based API calls
        shader_from_buffers (shadername);

    for (int i = 0;  i < argc;  i++) {
        inject_params ();
        shadernames.push_back (shadername);
        shadingsys->Shader (*shadergroup, "surface", shadername, layername);
        layername.clear ();
        params.clear ();
    }
    return 0;
}



static void
action_shaderdecl (int /*argc*/, const char *argv[])
{
    // `--shader shadername layername` is exactly equivalent to:
    // `--layer layername` followed by naming the shader.
    layername = argv[2];
    add_shader (1, argv+1);
}



// The --expr ARG command line option will take ARG that is a snipped of
// OSL source code, embed it in some boilerplate shader wrapper, compile
// it from memory, and run that in the same way that would have been done
// if it were a compiled shader on disk. The boilerplate assumes that there
// are two output parameters for the shader: color result, and float alpha.
//
// Example use:
//   testshade -v -g 64 64 -o result out.exr -expr 'result=color(u,v,0);'
//
static void
specify_expr (int argc OSL_MAYBE_UNUSED, const char *argv[])
{
    OSL_DASSERT(argc == 2);
    std::string shadername = OIIO::Strutil::sprintf("expr_%d", exprcount++);
    std::string sourcecode =
        "shader " + shadername + " (\n"
        "    float s = u [[ int lockgeom=0 ]],\n"
        "    float t = v [[ int lockgeom=0 ]],\n"
        "    output color result = 0,\n"
        "    output float alpha = 1,\n"
        "  )\n"
        "{\n"
        "    " + std::string(argv[1]) + "\n"
        "    ;\n"
        "}\n";
    if (verbose)
        std::cout << "Expression-based shader text is:\n---\n"
                  << sourcecode << "---\n";

    set_shadingsys_options ();

    compile_buffer (sourcecode, shadername);

    inject_params ();
    shadernames.push_back (shadername);
    shadingsys->Shader (*shadergroup, "surface", shadername, layername);
    layername.clear ();
    params.clear ();
}



// Parse str for `len` floats, separated by commas.
inline bool
parse_float_list(string_view str, float* f, int len)
{
    bool ok = true;
    for (int i = 0; i < len && ok; ++i) {
        ok &= OIIO::Strutil::parse_float(str, f[i]);
        if (ok && i < len - 1)
            ok &= OIIO::Strutil::parse_char(str, ',');
    }
    return ok;
}



// Utility: Add {paramname, stringval} to the given parameter list.
static void
add_param (ParamValueList& params, string_view command,
           string_view paramname, string_view stringval)
{
    TypeDesc type = TypeDesc::UNKNOWN;
    bool unlockgeom = false;
    float f[16];

    size_t pos;
    while ((pos = command.find_first_of(":")) != std::string::npos) {
        command = command.substr (pos+1, std::string::npos);
        std::vector<std::string> splits;
        OIIO::Strutil::split (command, splits, ":", 1);
        if (splits.size() < 1) {}
        else if (OIIO::Strutil::istarts_with(splits[0],"type="))
            type.fromstring (splits[0].c_str()+5);
        else if (OIIO::Strutil::istarts_with(splits[0],"lockgeom="))
            unlockgeom = (OIIO::Strutil::from_string<int> (splits[0]) == 0);
    }

    // If it is or might be a matrix, look for 16 comma-separated floats
    if ((type == TypeDesc::UNKNOWN || type == TypeDesc::TypeMatrix)
        && parse_float_list(stringval, f, 16)) {
        params.emplace_back (paramname, TypeDesc::TypeMatrix, 1, f);
        if (unlockgeom)
            params.back().interp (ParamValue::INTERP_VERTEX);
        return;
    }
    // If it is or might be a vector type, look for 3 comma-separated floats
    if ((type == TypeDesc::UNKNOWN || equivalent(type,TypeDesc::TypeVector))
        && parse_float_list(stringval, f, 3)) {
        if (type == TypeDesc::UNKNOWN)
            type = TypeDesc::TypeVector;
        params.emplace_back (paramname, type, 1, f);
        if (unlockgeom)
            params.back().interp (ParamValue::INTERP_VERTEX);
        return;
    }
    // If it is or might be an int, look for an int that takes up the whole
    // string.
    if ((type == TypeDesc::UNKNOWN || type == TypeDesc::TypeInt)
          && OIIO::Strutil::string_is<int>(stringval)) {
        params.emplace_back (paramname, OIIO::Strutil::from_string<int>(stringval));
        if (unlockgeom)
            params.back().interp (ParamValue::INTERP_VERTEX);
        return;
    }
    // If it is or might be an float, look for a float that takes up the
    // whole string.
    if ((type == TypeDesc::UNKNOWN || type == TypeDesc::TypeFloat)
          && OIIO::Strutil::string_is<float>(stringval)) {
        params.emplace_back (paramname, OIIO::Strutil::from_string<float>(stringval));
        if (unlockgeom)
            params.back().interp (ParamValue::INTERP_VERTEX);
        return;
    }

    // Catch-all for float types and arrays
    if (type.basetype == TypeDesc::FLOAT) {
        int n = type.aggregate * type.numelements();
        std::vector<float> vals (n);
        for (int i = 0;  i < n;  ++i) {
            OIIO::Strutil::parse_float (stringval, vals[i]);
            OIIO::Strutil::parse_char (stringval, ',');
        }
        params.emplace_back (paramname, type, 1, &vals[0]);
        if (unlockgeom)
            params.back().interp (ParamValue::INTERP_VERTEX);
        return;
    }

    // Catch-all for int types and arrays
    if (type.basetype == TypeDesc::INT) {
        int n = type.aggregate * type.numelements();
        std::vector<int> vals (n);
        for (int i = 0;  i < n;  ++i) {
            OIIO::Strutil::parse_int (stringval, vals[i]);
            OIIO::Strutil::parse_char (stringval, ',');
        }
        params.emplace_back (paramname, type, 1, &vals[0]);
        if (unlockgeom)
            params.back().interp (ParamValue::INTERP_VERTEX);
        return;
    }

    // String arrays are slightly tricky
    if (type.basetype == TypeDesc::STRING && type.is_array()) {
        std::vector<string_view> splitelements;
        OIIO::Strutil::split (stringval, splitelements, ",", type.arraylen);
        splitelements.resize (type.arraylen);
        std::vector<ustring> strelements;
        for (auto&& s : splitelements)
            strelements.push_back (ustring(s));
        params.emplace_back (paramname, type, 1, &strelements[0]);
        if (unlockgeom)
            params.back().interp (ParamValue::INTERP_VERTEX);
        return;
    }

    // All remaining cases -- it's a string
    const char *s = stringval.c_str();
    params.emplace_back (paramname, TypeDesc::TypeString, 1, &s);
    if (unlockgeom)
        params.back().interp (ParamValue::INTERP_VERTEX);
}



static void
action_param(int /*argc*/, const char *argv[])
{
    std::string command = argv[0];
    bool use_reparam = false;
    if (OIIO::Strutil::istarts_with(command, "--reparam") ||
        OIIO::Strutil::istarts_with(command, "-reparam"))
        use_reparam = true;
    ParamValueList &params (use_reparam ? reparams : (::params));

    add_param(params, command, argv[1], argv[2]);
}



// reparam -- just set reparam_layer and then let action_param do all the
// hard work.
static void
action_reparam (int /*argc*/, const char *argv[])
{
    reparam_layer = argv[1];
    const char *newargv[] = { argv[0], argv[2], argv[3] };
    action_param (3, newargv);
}



static void
action_groupspec (int /*argc*/, const char *argv[])
{
    shadingsys->ShaderGroupEnd (*shadergroup);
    std::string groupspec (argv[1]);
    if (OIIO::Filesystem::exists (groupspec)) {
        // If it names a file, use the contents of the file as the group
        // specification.
        OIIO::Filesystem::read_text_file (groupspec, groupspec);
    }
    set_shadingsys_options ();
    if (verbose)
        std::cout << "Processing group specification:\n---\n"
                  << groupspec << "\n---\n";
    shadergroup = shadingsys->ShaderGroupBegin (groupname, "surface", groupspec);
}



static void
stash_shader_arg (int argc, const char* argv[])
{
    for (int i = 0; i < argc; ++i)
        shader_setup_args.push_back (argv[i]);
}



static void
stash_userdata(int argc, const char* argv[])
{
    add_param(userdata, argv[0], argv[1], argv[2]);
}



void
print_info()
{
    ErrorHandler errhandler;
    SimpleRenderer* rend = nullptr;
#ifdef OSL_USE_OPTIX
    if (use_optix)
        rend = new OptixGridRenderer;
    else
#endif
        rend = new SimpleRenderer;
    TextureSystem *texturesys = TextureSystem::create();
    shadingsys = new ShadingSystem(rend, texturesys, &errhandler);
    rend->init_shadingsys(shadingsys);

    std::cout << "\n" << shadingsys->getstats (5) << "\n";

    delete shadingsys;
    delete rend;
}



static void
getargs (int argc, const char *argv[])
{
    static bool help = false;

    // We have a bit of a chicken-and-egg problem here, where some arguments
    // set up the shader instances, but other args and housekeeping are
    // needed first. Untangle by just storing the shader setup args until
    // they can be later processed in full.
    shader_setup_args.clear();
    shader_setup_args.push_back("testshade"); // seed with 'program'

    OIIO::ArgParse ap;
    ap.options ("Usage:  testshade [options] shader...",
                "%*", stash_shader_arg, "",
                "--help", &help, "Print help message",
                "-v", &verbose, "Verbose messages",
                "-t %d", &num_threads, "Render using N threads (default: auto-detect)",
                "--optix", &use_optix, "Use OptiX if available",
                "--debug", &debug1, "Lots of debugging info",
                "--debug2", &debug2, "Even more debugging info",
                "--llvm_debug", &llvm_debug, "Turn on LLVM debugging info",
                "--runstats", &runstats, "Print run statistics",
                "--stats", &runstats, "",  // DEPRECATED 1.7
                "--batched", &batched, "Submit batches to ShadingSystem",
                "--vary_pdxdy", &vary_Pdxdy, "populate Dx(P) & Dy(P) with varying values (vs. uniform)",
                "--vary_udxdy", &vary_udxdy, "populate Dx(u) & Dy(u) with varying values (vs. uniform)",
                "--vary_vdxdy", &vary_vdxdy, "populate Dx(v) & Dy(v) with varying values (vs. uniform)",
                "--profile", &profile, "Print profile information",
                "--saveptx", &saveptx, "Save the generated PTX (OptiX mode only)",
                "--warmup", &warmup, "Perform a warmup launch",
                "--res %d %d", &xres, &yres, "Make an W x H image",
                "-g %d %d", &xres, &yres, "", // synonym for -res
                "--options %s", &extraoptions, "Set extra OSL options",
                "--texoptions %s", &texoptions, "Set extra TextureSystem options",
                "-o %L %L", &outputvars, &outputfiles,
                        "Output (variable, filename)   [filename='null' means don't save]",
                "-d %s", &dataformatname, "Set the output data format to one of: "
                        "uint8, half, float",
                "-od %s", &dataformatname, "", // old name
                "--print", &print_outputs, "Print values of all -o outputs to console instead of saving images",
                "--groupname %s", &groupname, "Set shader group name",
                "--layer %@ %s", stash_shader_arg, NULL, "Set next layer name",
                "--param %@ %s %s", stash_shader_arg, NULL, NULL,
                        "Add a parameter (args: name value) (options: type=%s, lockgeom=%d)",
                "--shader %@ %s %s", stash_shader_arg, NULL, NULL,
                        "Declare a shader node (args: shader layername)",
                "--connect %@ %s %s %s %s",
                    stash_shader_arg, NULL, NULL, NULL, NULL,
                    "Connect fromlayer fromoutput tolayer toinput",
                "--reparam %@ %s %s %s", stash_shader_arg, NULL, NULL, NULL,
                        "Change a parameter (args: layername paramname value) (options: type=%s)",
                "--group %@ %s", stash_shader_arg, NULL,
                        "Specify a full group command",
                "--archivegroup %s", &archivegroup,
                        "Archive the group to a given filename",
                "--raytype %s", &raytype, "Set the raytype",
                "--raytype_opt", &raytype_opt, "Specify ray type mask for optimization",
                "--iters %d", &iters, "Number of iterations",
                "-O0", &O0, "Do no runtime shader optimization",
                "-O1", &O1, "Do a little runtime shader optimization",
                "-O2", &O2, "Do lots of runtime shader optimization",
                "--llvm_opt %d", &llvm_opt, "LLVM JIT optimization level",
                "--entry %L", &entrylayers, "Add layer to the list of entry points",
                "--entryoutput %L", &entryoutputs, "Add output symbol to the list of entry points",
                "--center", &pixelcenters, "Shade at output pixel 'centers' rather than corners",
                "--debugnan", &debugnan, "Turn on 'debug_nan' mode",
                "--debuguninit", &debug_uninit, "Turn on 'debug_uninit' mode",
                "--groupoutputs", &use_group_outputs, "Specify group outputs, not global outputs",
                "--oslquery", &do_oslquery, "Test OSLQuery at runtime",
                "--inbuffer", &inbuffer, "Compile osl source from and to buffer",
                "--no-output-placement %!", &output_placement,
                        "Turn off use of output placement, rely only on get_symbol",
                "--shadeimage", &use_shade_image, "Use shade_image utility",
                "--noshadeimage %!", &use_shade_image, "Don't use shade_image utility",
                "--expr %@ %s", stash_shader_arg, NULL, "Specify an OSL expression to evaluate",
                "--offsetuv %f %f", &uoffset, &voffset, "Offset s & t texture coordinates (default: 0 0)",
                "--offsetst %f %f", &uoffset, &voffset, "", // old name
                "--scaleuv %f %f", &uscale, &vscale, "Scale s & t texture lookups (default: 1, 1)",
                "--scalest %f %f", &uscale, &vscale, "", // old name
                "--userdata %@ %s %s", stash_userdata, nullptr, nullptr,
                        "Add userdata (args: name value) (options: type=%s)",
                "--userdata_isconnected", &userdata_isconnected, "Consider lockgeom=0 to be isconnected()",
                "--locale %s", &localename, "Set a different locale",
                NULL);
    if (ap.parse(argc, argv) < 0 /*|| (shadernames.empty() && groupspec.empty())*/) {
        std::cerr << ap.geterror() << std::endl;
        ap.usage ();
        exit (EXIT_FAILURE);
    }
    if (help) {
        std::cout << "testshade -- Test Open Shading Language\n"
                     OSL_COPYRIGHT_STRING "\n";
        ap.usage ();
        print_info();
        exit (EXIT_SUCCESS);
    }
}



static void
process_shader_setup_args (int argc, const char *argv[])
{
    OIIO::ArgParse ap;
    ap.options ("Usage:  testshade [options] shader...",
                "%*", add_shader, "",
                "--layer %s", &layername, "Set next layer name",
                "--param %@ %s %s", &action_param, NULL, NULL,
                        "Add a parameter (args: name value) (options: type=%s, lockgeom=%d)",
                "--shader %@ %s %s", &action_shaderdecl, NULL, NULL,
                        "Declare a shader node (args: shader layername)",
                "--connect %L %L %L %L",
                    &connections, &connections, &connections, &connections,
                    "Connect fromlayer fromoutput tolayer toinput",
                "--reparam %@ %s %s %s", &action_reparam, NULL, NULL, NULL,
                        "Change a parameter (args: layername paramname value) (options: type=%s)",
                "--group %@ %s", &action_groupspec, &groupspec,
                        "Specify a full group command",
                "--expr %@ %s", &specify_expr, NULL, "Specify an OSL expression to evaluate",
                NULL);
    if (ap.parse(argc, argv) < 0 || (shadernames.empty() && groupspec.empty())) {
        std::cerr << "ERROR: No shader or group was specified.\n";
        std::cerr << ap.geterror() << std::endl;
        std::cerr << "Try `testshade --help` for an explanation of all arguments\n";
        exit (EXIT_FAILURE);
    }
}



// Here we set up transformations.  These are just examples, set up so
// that our unit tests can transform among spaces in ways that we will
// recognize as correct.  The "shader" and "object" spaces are required
// by OSL and the ShaderGlobals will need to have references to them.
// For good measure, we also set up a "myspace" space, registering it
// with the RendererServices.
// 
static void
setup_transformations (SimpleRenderer &rend, OSL::Matrix44 &Mshad,
                       OSL::Matrix44 &Mobj)
{
    Matrix44 M (1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    rend.camera_params (M, ustring("perspective"), 90.0f,
                        0.1f, 1000.0f, xres, yres);

    // Make a "shader" space that is translated one unit in x and rotated
    // 45deg about the z axis.
    Mshad.makeIdentity ();
    Mshad.translate (OSL::Vec3 (1.0, 0.0, 0.0));
    Mshad.rotate (OSL::Vec3 (0.0, 0.0, M_PI_4));
    // std::cout << "shader-to-common matrix: " << Mshad << "\n";

    // Make an object space that is translated one unit in y and rotated
    // 90deg about the z axis.
    Mobj.makeIdentity ();
    Mobj.translate (OSL::Vec3 (0.0, 1.0, 0.0));
    Mobj.rotate (OSL::Vec3 (0.0, 0.0, M_PI_2));
    // std::cout << "object-to-common matrix: " << Mobj << "\n";

    OSL::Matrix44 Mmyspace;
    Mmyspace.scale (OSL::Vec3 (1.0, 2.0, 1.0));
    // std::cout << "myspace-to-common matrix: " << Mmyspace << "\n";
    rend.name_transform ("myspace", Mmyspace);
}



// Set up the ShaderGlobals fields for pixel (x,y).
static void
setup_shaderglobals (ShaderGlobals &sg, ShadingSystem *shadingsys,
                     int x, int y)
{
    // Just zero the whole thing out to start
    memset ((char *)&sg, 0, sizeof(ShaderGlobals));

    // In our SimpleRenderer, the "renderstate" itself just a pointer to
    // the ShaderGlobals.
    sg.renderstate = &sg;

    // Set "shader" space to be Mshad.  In a real renderer, this may be
    // different for each shader group.
    sg.shader2common = OSL::TransformationPtr (&Mshad);

    // Set "object" space to be Mobj.  In a real renderer, this may be
    // different for each object.
    sg.object2common = OSL::TransformationPtr (&Mobj);

    // Just make it look like all shades are the result of 'raytype' rays.
    sg.raytype = shadingsys->raytype_bit (ustring (raytype));

    // Set up u,v to vary across the "patch", and also their derivatives.
    // Note that since u & x, and v & y are aligned, we only need to set
    // values for dudx and dvdy, we can use the memset above to have set
    // dvdx and dudy to 0.
    if (pixelcenters) {
        // Our patch is like an "image" with shading samples at the
        // centers of each pixel.
        sg.u = uscale * (float)(x+0.5f) / xres + uoffset;
        sg.v = vscale * (float)(y+0.5f) / yres + voffset;
        if (vary_udxdy) {
            sg.dudx = 1.0f - sg.u;
            sg.dudy = sg.u;
        } else {
            sg.dudx = uscale / xres;
        }
        if (vary_vdxdy) {
            sg.dvdx = 1.0f - sg.v;
            sg.dvdy = sg.v;
        } else {
            sg.dvdy = vscale / yres;
        }
    } else {
        // Our patch is like a Reyes grid of points, with the border
        // samples being exactly on u,v == 0 or 1.
        sg.u = uscale * ((xres == 1) ? 0.5f : (float) x / (xres - 1)) + uoffset;
        sg.v = vscale * ((yres == 1) ? 0.5f : (float) y / (yres - 1)) + voffset;
        if (vary_udxdy) {
            sg.dudx = 1.0f - sg.u;
            sg.dudy = sg.u;
        } else {
            sg.dudx = uscale / std::max (1, xres-1);
        }
        if (vary_vdxdy) {
            sg.dvdx = 1.0f - sg.v;
            sg.dvdy = sg.v;
        } else {
            sg.dvdy = vscale / std::max (1, yres-1);
        }
    }

    // Assume that position P is simply (u,v,1), that makes the patch lie
    // on [0,1] at z=1.
    sg.P = Vec3 (sg.u, sg.v, 1.0f);
    // Derivatives with respect to x,y
    if (vary_Pdxdy) {
        sg.dPdx = Vec3 (1.0f - sg.u, 1.0f - sg.v, sg.u*0.5);
        sg.dPdy = Vec3 (1.0f - sg.v, 1.0f - sg.u, sg.v*0.5);
    } else {
        sg.dPdx = Vec3 (uscale / std::max (1, xres-1), 0.0f, 0.0f);
        sg.dPdy = Vec3 (0.0f, vscale / std::max (1, yres-1), 0.0f);
    }
    sg.dPdz = Vec3 (0.0f, 0.0f, 0.0f);  // just use 0 for volume tangent
    // Tangents of P with respect to surface u,v
    sg.dPdu = Vec3 (1.0f, 0.0f, 0.0f);
    sg.dPdv = Vec3 (0.0f, 1.0f, 0.0f);
    // That also implies that our normal points to (0,0,1)
    sg.N    = Vec3 (0, 0, 1);
    sg.Ng   = Vec3 (0, 0, 1);

    // Set the surface area of the patch to 1 (which it is).  This is
    // only used for light shaders that call the surfacearea() function.
    sg.surfacearea = 1;
}



static void
setup_output_images (SimpleRenderer *rend, ShadingSystem *shadingsys,
                     ShaderGroupRef &shadergroup)
{
    // If the command line didn't specify any outputs, default to Cout.
    if (! outputvars.size()) {
        outputvars.emplace_back("Cout");
        outputfiles.emplace_back("null");
    }

    // Declare entry layers, if specified
    // N.B. Maybe nobody cares about running individual layers manually,
    // and all this entry layer output nonsense can go away.
    if (entrylayers.size()) {
        std::vector<const char *> layers;
        std::cout << "Entry layers:";
        for (size_t i = 0; i < entrylayers.size(); ++i) {
            ustring layername (entrylayers[i]);  // convert to ustring
            int layid = shadingsys->find_layer (*shadergroup, layername);
            layers.push_back (layername.c_str());
            entrylayer_index.push_back (layid);
            std::cout << ' ' << entrylayers[i] << "(" << layid << ")";
        }
        std::cout << "\n";
        shadingsys->attribute (shadergroup.get(), "entry_layers",
                               TypeDesc(TypeDesc::STRING,(int)entrylayers.size()),
                               &layers[0]);
    }

    // Get info about the number of layers in the shader group
    int num_layers = 0;
    shadingsys->getattribute(shadergroup.get(), "num_layers", num_layers);
    std::vector<ustring> layernames(num_layers);
    if (num_layers)
        shadingsys->getattribute(shadergroup.get(), "layer_names",
                                 TypeDesc(TypeDesc::STRING, num_layers),
                                 &layernames[0]);


    // For each output file specified on the command line, figure out if
    // it's really an output of some layer (and its type), and tell the
    // renderer that it's an output.
    for (size_t i = 0;  i < outputfiles.size();  ++i) {
        auto pieces = OIIO::Strutil::splitsv(outputvars[i], ".", 2);
        string_view layer(pieces.size() > 1 ? pieces.front() : string_view());
        string_view var(pieces.back());
        TypeDesc vartype;
        bool found = false;
        // We need to walk the layers and find out the type of this output.
        // This complexity is only because we allow the command line to
        // specifify outputs by name only. Go back to front so if the name
        // we were given doesn't designate a layer, we preferentially find
        // it at the end.
        // std::cout << "Considering " << outputfiles[i] << " - " << outputvars[i] << "\n";
        // std::cout << "  seeking layer=" << layer << " var=" << var << "\n";
        for (int lay = num_layers - 1; lay >= 0 && !found; --lay) {
            // std::cout << "   layer " << lay << " " << layernames[lay] << "\n";
            if (layer == layernames[lay] || layer.empty()) {
                OSLQuery oslquery = shadingsys->oslquery(*shadergroup, lay);
                for (const auto& param : oslquery) {
                    // std::cout << "    param " << param.type << " " << param.name
                    //           << " isoutput=" << param.isoutput << "\n";
                    if (param.isoutput && param.name == var) {
                        // std::cout << "    found param " << param.name << "\n";
                        vartype = param.type;
                        found = true;
                        break;
                    }
                }
            }
        }
        if (found) {
            outputvarnames.emplace_back(var);  // ?? outputvars[i]
            outputvartypes.emplace_back(vartype);
            if (outputfiles[i] != "null")
                std::cout << "Output " << outputvars[i] << " to "
                          << outputfiles[i] << "\n";

            TypeDesc tbase((TypeDesc::BASETYPE)vartype.basetype);
            int nchans = vartype.basevalues();

            // Make an ImageBuf of the right type and size to hold this
            // symbol's output, and initially clear it to all black pixels.
            rend->add_output (outputvars[i], outputfiles[i], tbase, nchans);
        }
    }

    if (output_placement && rend->noutputs()) {
        // Set up SymLocationDesc for the outputs
        std::vector<SymLocationDesc> symlocs;
        for (size_t i = 0; i < rend->noutputs(); ++i) {
            OIIO::ImageBuf* ib = rend->outputbuf(i);
            char* outptr = static_cast<char*>(ib->pixeladdr(0,0));
            if (i == 0) {
                // The output arena is the start of the first output buffer
                output_base_ptr = outptr;
            }
            ptrdiff_t offset = outptr - output_base_ptr;
            TypeDesc t = outputvartypes[i];
            symlocs.emplace_back(outputvars[i], t, /*derivs*/ false,
                                 SymArena::Outputs, offset,
                                 /*stride*/ t.size());
            // std::cout.flush();
            // OIIO::Strutil::print("  symloc {} {} off={} size={}\n",
            //                      outputvars[i], t, offset, t.size());
        }
        shadingsys->add_symlocs(shadergroup.get(), symlocs);
    }

    if (!output_placement && outputvars.size()) {
        // Old fashined way -- tell the shading system which outputs we want
        std::vector<const char *> aovnames (outputvars.size());
        for (size_t i = 0; i < outputvars.size(); ++i) {
            ustring varname (outputvars[i]);
            aovnames[i] = varname.c_str();
            size_t dot = varname.find('.');
            if (dot != ustring::npos) {
                // If the name contains a dot, it's intended to be layer.symbol
                varname = ustring (varname, dot+1);
            }
        }
        shadingsys->attribute (use_group_outputs ? shadergroup.get() : NULL,
                               "renderer_outputs",
                               TypeDesc(TypeDesc::STRING,(int)aovnames.size()),
                               &aovnames[0]);
#if 0
        // TODO:  Why would we output this when only !output_placement?
        //        disabling because causing differences in testsuite results
        if (use_group_outputs)
            std::cout << "Marking group outputs, not global renderer outputs.\n";
#endif
    }

    // N.B. Maybe nobody cares about running individual layers manually,
    // and all this entry layer output nonsense can go away.
    if (entryoutputs.size()) {
        // Because we can only call find_symbol or get_symbol on something that
        // has been set up to shade (or executed), we call execute() but tell it
        // not to actually run the shader.
        OSL::PerThreadInfo *thread_info = shadingsys->create_thread_info();
        ShadingContext *ctx = shadingsys->get_context(thread_info);
        ShaderGlobals sg;
        setup_shaderglobals (sg, shadingsys, 0, 0);

        int raytype_bit = shadingsys->raytype_bit (ustring (raytype));
#if OSL_USE_BATCHED
        if (batched) {
            // jit_group will optimize the group if necesssary
            if (batch_size == 16) {
                shadingsys->batched<16>().jit_group (shadergroup.get(), ctx);
            } else {
                ASSERT((batch_size == 8) && "Unsupport batch size");
                shadingsys->batched<8>().jit_group (shadergroup.get(), ctx);
            }
        } else
#endif
        if (raytype_opt)
            shadingsys->optimize_group (shadergroup.get(), raytype_bit, ~raytype_bit, ctx);
        shadingsys->execute (*ctx, *shadergroup, sg, false);
        std::cout << "Entry outputs:";
        for (size_t i = 0; i < entryoutputs.size(); ++i) {
            ustring name (entryoutputs[i]);  // convert to ustring
            const ShaderSymbol *sym = shadingsys->find_symbol (*shadergroup, name);
            if (!sym) {
                std::cout << "\nEntry output " << entryoutputs[i] << " not found. Abording.\n";
                exit (EXIT_FAILURE);
            }
            entrylayer_symbols.push_back (sym);
            std::cout << ' ' << entryoutputs[i];
        }
        std::cout << "\n";
        shadingsys->release_context (ctx);  // don't need this anymore for now
        shadingsys->destroy_thread_info(thread_info);
    }
}



// For pixel (x,y) that was just shaded by the given shading context,
// save each of the requested outputs to the corresponding output
// ImageBuf.
//
// In a real renderer, this is illustrative of how you would pull shader
// outputs into "AOV's" (arbitrary output variables, or additional
// renderer outputs).  You would, of course, also grab the closure Ci
// and integrate the lights using that BSDF to determine the radiance
// in the direction of the camera for that pixel.
static void
save_outputs (SimpleRenderer *rend, ShadingSystem *shadingsys,
              ShadingContext *ctx, int x, int y)
{
    using OIIO::Strutil::printf;
    if (print_outputs)
        printf ("Pixel (%d, %d):\n", x, y);
    // For each output requested on the command line...
    for (size_t i = 0, e = rend->noutputs();  i < e;  ++i) {
        // Skip if we couldn't open the image or didn't match a known output
        OIIO::ImageBuf* outputimg = rend->outputbuf(i);
        if (! outputimg)
            continue;

        // Ask for a pointer to the symbol's data, as computed by this
        // shader.
        TypeDesc t;
        const void *data = shadingsys->get_symbol (*ctx, rend->outputname(i), t);
        if (!data)
            continue;  // Skip if symbol isn't found

        int nchans = outputimg->nchannels();
        if (t.basetype == TypeDesc::FLOAT) {
            // If the variable we are outputting is float-based, set it
            // directly in the output buffer.
            outputimg->setpixel (x, y, (const float *)data);
            if (print_outputs) {
                printf ("  %s :", outputvarnames[i]);
                for (int c = 0; c < nchans; ++c)
                    printf (" %g", ((const float *)data)[c]);
                printf ("\n");
            }
        } else if (t.basetype == TypeDesc::INT) {
            // We are outputting an integer variable, so we need to
            // convert it to floating point.
            float *pixel = OIIO_ALLOCA(float, nchans);
            OIIO::convert_types (TypeDesc::BASETYPE(t.basetype), data,
                                 TypeDesc::FLOAT, pixel, nchans);
            outputimg->setpixel (x, y, &pixel[0]);
            if (print_outputs) {
                printf ("  %s :", outputvarnames[i]);
                for (int c = 0; c < nchans; ++c)
                    printf (" %d", ((const int *)data)[c]);
                printf ("\n");
            }
        }
        // N.B. Drop any outputs that aren't float- or int-based
    }
}


#if OSL_USE_BATCHED

// For batch of pixels (bx[WidthT], by[WidthT]) that was just shaded
// by the given shading context, save each of the requested outputs
// to the corresponding output ImageBuf.
//
// In a real renderer, this is illustrative of how you would pull shader
// outputs into "AOV's" (arbitrary output variables, or additional
// renderer outputs).  You would, of course, also grab the closure Ci
// and integrate the lights using that BSDF to determine the radiance
// in the direction of the camera for that pixel.
template<int WidthT>
static void
batched_save_outputs (SimpleRenderer *rend, ShadingSystem *shadingsys, ShadingContext *ctx, ShaderGroup *shadergroup, int batchSize, int (&bx)[WidthT], int (&by)[WidthT])
{
    OSL_ASSERT(batchSize <= WidthT);
    // Because we are choosing to loop over outputs then over the batch
    // we will need to keep separate output streams for each batch
    // to prevent multiplexing
    std::unique_ptr<std::stringstream> oStreams[WidthT];
    if (print_outputs) {
        for(int batchIndex=0; batchIndex < batchSize; ++batchIndex) {
            oStreams[batchIndex].reset(new std::stringstream());
            int x = bx[batchIndex];
            int y = by[batchIndex];
            *oStreams[batchIndex] << "Pixel (" << x << ", " << y << "):" << std::endl;
        }
    }

    // In batched mode, a symbol's address can be passed to the contructor of the
    // lightweight data adapter:
    // template <typename DataT, int WidthT> struct Wide
    // which provides the array subscript accessor to access DataT for each batchIndex

    // For each output requested on the command line...
    for (size_t i = 0, e = rend->noutputs();  i < e;  ++i) {
        // Skip if we couldn't open the image or didn't match a known output
        OIIO::ImageBuf* outputimg = rend->outputbuf(i);
        if (! outputimg)
            continue;

        const ShaderSymbol* out_symbol = shadingsys->find_symbol (*shadergroup, rend->outputname(i));
        if (!out_symbol)
            continue;  // Skip if symbol isn't found

        TypeDesc t = shadingsys->symbol_typedesc(out_symbol);
        int nchans = outputimg->nchannels();

        // Used Wide access on the symbol's data t access per lane results
        if (t.basetype == TypeDesc::FLOAT) {
            // If the variable we are outputting is float-based, set it
            // directly in the output buffer.
            if (t.aggregate == TypeDesc::MATRIX44) {
                OSL_DASSERT(nchans == 16);
                Wide<const Matrix44, WidthT> batchResults(shadingsys->symbol_address(*ctx, out_symbol));
                for(int batchIndex=0; batchIndex < batchSize; ++batchIndex) {
                    int x = bx[batchIndex];
                    int y = by[batchIndex];
                    Matrix44 data = batchResults[batchIndex];
                    outputimg->setpixel (x, y, reinterpret_cast<const float *>(&data));
                    if (print_outputs) {
                        // Match the scalar save_outputs behavior of outputting
                        // each component without surrounding parenthesis we
                        // get with << operator
                        //*oStreams[batchIndex] << "  " << outputvarnames[i].c_str() << " :" << data << std::endl;
                        *oStreams[batchIndex] << "  " << outputvarnames[i].c_str() << " :"
                            << " " << data.x[0][0] << " " << data.x[0][1] << " " << data.x[0][2] << " " << data.x[0][3]
                            << " " << data.x[1][0] << " " << data.x[1][1] << " " << data.x[1][2] << " " << data.x[3][3]
                            << " " << data.x[2][0] << " " << data.x[2][1] << " " << data.x[2][2] << " " << data.x[3][3]
                            << " " << data.x[3][0] << " " << data.x[3][1] << " " << data.x[3][2] << " " << data.x[3][3]
                            << std::endl;
                    }
                }
            }
            if (t.aggregate == TypeDesc::VEC3) {
                OSL_DASSERT(nchans == 3);
                Wide<const Vec3, WidthT> batchResults(shadingsys->symbol_address(*ctx, out_symbol));
                for(int batchIndex=0; batchIndex < batchSize; ++batchIndex) {
                    int x = bx[batchIndex];
                    int y = by[batchIndex];
                    Vec3 data = batchResults[batchIndex];
                    outputimg->setpixel (x, y, reinterpret_cast<const float *>(&data));
                    if (print_outputs) {
                        // Match the scalar save_outputs behavior of outputting
                        // each component without surrounding parenthesis we
                        // get with << operator
                        //*oStreams[batchIndex] << "  " << outputvarnames[i].c_str() << " :" << data << std::endl;
                        *oStreams[batchIndex] << "  " << outputvarnames[i].c_str() << " : " << data.x << " " << data.y << " " << data.z << std::endl;
                    }
                }
            }
            if (t.aggregate == TypeDesc::SCALAR) {
                OSL_DASSERT(nchans == 1);
                Wide<const float, WidthT> batchResults(shadingsys->symbol_address(*ctx, out_symbol));
                for(int batchIndex=0; batchIndex < batchSize; ++batchIndex) {
                    int x = bx[batchIndex];
                    int y = by[batchIndex];
                    float data = batchResults[batchIndex];
                    outputimg->setpixel (x, y, reinterpret_cast<const float *>(&data));
                    if (print_outputs) {
                        *oStreams[batchIndex] << "  " << outputvarnames[i].c_str() << " :" << data << std::endl;
                    }
                }
            }
        } else if (t.basetype == TypeDesc::INT) {
            // We are outputting an integer variable, so we need to
            // convert it to floating point.
            if (nchans == 1) {
                Wide<const int, WidthT> batchResults(shadingsys->symbol_address(*ctx, out_symbol));
                for(int batchIndex=0; batchIndex < batchSize; ++batchIndex) {
                    int x = bx[batchIndex];
                    int y = by[batchIndex];
                    int data = batchResults[batchIndex];
                    float pixel[1];
                    OIIO::convert_types (TypeDesc::BASETYPE(t.basetype), &data,
                                         TypeDesc::FLOAT, &pixel[0], 1 /*nchans*/);
                    outputimg->setpixel (x, y, &pixel[0]);
                    if (print_outputs) {
                        *oStreams[batchIndex] << "  " << outputvarnames[i].c_str() << " :" << data << std::endl;
                    }
                }
            } else {
                // We don't expect this to happen, but leaving as example for others
                Wide<const int[], WidthT> batchResults(shadingsys->symbol_address(*ctx, out_symbol), nchans);
                // TODO:  Try not to do alloca's inside a loop
                int *intPixel = OIIO_ALLOCA(int, nchans);
                float *floatPixel = OIIO_ALLOCA(float, nchans);
                for(int batchIndex=0; batchIndex < batchSize; ++batchIndex) {
                    int x = bx[batchIndex];
                    int y = by[batchIndex];
                    for(int c=0; c < nchans; ++c) {
                        intPixel[c] = batchResults[batchIndex][c];
                    }

                    OIIO::convert_types (TypeDesc::BASETYPE(t.basetype), intPixel,
                                         TypeDesc::FLOAT, floatPixel, 3 /*nchans*/);
                    outputimg->setpixel (x, y, floatPixel);
                    if (print_outputs) {
                        (*oStreams[batchIndex]) << "  " << outputvarnames[i].c_str() << " :" ;
                        for (int c = 0; c < nchans; ++c)
                            (*oStreams[batchIndex]) << " " << (intPixel[c]);
                        *oStreams[batchIndex] << std::endl;
                    }
                }
            }

        }
        // N.B. Drop any outputs that aren't float- or int-based
    }

    if (print_outputs) {
        // Serialize multiple output streams of the batch
        for(int batchIndex=0; batchIndex < batchSize; ++batchIndex) {
            std::cout << oStreams[batchIndex]->str();
        }
    }
}
#endif



static void
test_group_attributes (ShaderGroup *group)
{
    int nt = 0;
    if (shadingsys->getattribute (group, "num_textures_needed", nt)) {
        std::cout << "Need " << nt << " textures:\n";
        ustring *tex = NULL;
        shadingsys->getattribute (group, "textures_needed",
                                  TypeDesc::PTR, &tex);
        for (int i = 0; i < nt; ++i)
            std::cout << "    " << tex[i] << "\n";
        int unk = 0;
        shadingsys->getattribute (group, "unknown_textures_needed", unk);
        if (unk)
            std::cout << "    and unknown textures\n";
    }
    int nclosures = 0;
    if (shadingsys->getattribute (group, "num_closures_needed", nclosures)) {
        std::cout << "Need " << nclosures << " closures:\n";
        ustring *closures = NULL;
        shadingsys->getattribute (group, "closures_needed",
                                  TypeDesc::PTR, &closures);
        for (int i = 0; i < nclosures; ++i)
            std::cout << "    " << closures[i] << "\n";
        int unk = 0;
        shadingsys->getattribute (group, "unknown_closures_needed", unk);
        if (unk)
            std::cout << "    and unknown closures\n";
    }
    int nglobals = 0;
    if (shadingsys->getattribute (group, "num_globals_needed", nglobals)) {
        std::cout << "Need " << nglobals << " globals: ";
        ustring *globals = NULL;
        shadingsys->getattribute (group, "globals_needed",
                                  TypeDesc::PTR, &globals);
        for (int i = 0; i < nglobals; ++i)
            std::cout << " " << globals[i];
        std::cout << "\n";
    }

    int globals_read = 0;
    int globals_write = 0;
    shadingsys->getattribute (group, "globals_read", globals_read);
    shadingsys->getattribute (group, "globals_write", globals_write);
    std::cout << "Globals read: (" << globals_read << ") ";
    for (int i = 1; i < int(SGBits::last); i <<= 1)
        if (globals_read & i)
            std::cout << ' ' << shadingsys->globals_name (SGBits(i));
    std::cout << "\nGlobals written: (" << globals_write << ") ";
    for (int i = 1; i < int(SGBits::last); i <<= 1)
        if (globals_write & i)
            std::cout << ' ' << shadingsys->globals_name (SGBits(i));
    std::cout << "\n";

    int nuser = 0;
    if (shadingsys->getattribute (group, "num_userdata", nuser) && nuser) {
        std::cout << "Need " << nuser << " user data items:\n";
        ustring *userdata_names = NULL;
        TypeDesc *userdata_types = NULL;
        int *userdata_offsets = NULL;
        bool *userdata_derivs = NULL;
        shadingsys->getattribute (group, "userdata_names",
                                  TypeDesc::PTR, &userdata_names);
        shadingsys->getattribute (group, "userdata_types",
                                  TypeDesc::PTR, &userdata_types);
        shadingsys->getattribute (group, "userdata_offsets",
                                  TypeDesc::PTR, &userdata_offsets);
        shadingsys->getattribute (group, "userdata_derivs",
                                  TypeDesc::PTR, &userdata_derivs);
        OSL_DASSERT(userdata_names && userdata_types && userdata_offsets);
        for (int i = 0; i < nuser; ++i)
            std::cout << "    " << userdata_names[i] << ' '
                      << userdata_types[i] << "  offset="
                      << userdata_offsets[i] << " deriv="
                      << userdata_derivs[i] << "\n";
    }
    int nattr = 0;
    if (shadingsys->getattribute (group, "num_attributes_needed", nattr) && nattr) {
        std::cout << "Need " << nattr << " attributes:\n";
        ustring *names = NULL;
        ustring *scopes = NULL;
        shadingsys->getattribute (group, "attributes_needed",
                                  TypeDesc::PTR, &names);
        shadingsys->getattribute (group, "attribute_scopes",
                                  TypeDesc::PTR, &scopes);
        OSL_DASSERT(names && scopes);
        for (int i = 0; i < nattr; ++i)
            std::cout << "    " << names[i] << ' '
                      << scopes[i] << "\n";

        int unk = 0;
        shadingsys->getattribute (group, "unknown_attributes_needed", unk);
        if (unk)
            std::cout << "    and unknown attributes\n";
    }
    int raytype_queries = 0;
    shadingsys->getattribute (group, "raytype_queries", raytype_queries);
    std::cout << "raytype() query mask: " << raytype_queries << "\n";
}



void
shade_region (SimpleRenderer *rend, ShaderGroup *shadergroup,
              OIIO::ROI roi, bool save)
{
    // Request an OSL::PerThreadInfo for this thread.
    OSL::PerThreadInfo *thread_info = shadingsys->create_thread_info();

    // Request a shading context so that we can execute the shader.
    // We could get_context/release_context for each shading point,
    // but to save overhead, it's more efficient to reuse a context
    // within a thread.
    ShadingContext *ctx = shadingsys->get_context (thread_info);

    // Set up shader globals and a little test grid of points to shade.
    ShaderGlobals shaderglobals;

    // Loop over all pixels in the image (in x and y)...
    for (int y = roi.ybegin;  y < roi.yend;  ++y) {
        int shadeindex = y * xres + roi.xbegin;
        for (int x = roi.xbegin;  x < roi.xend;  ++x, ++shadeindex) {
            // In a real renderer, this is where you would figure
            // out what object point is visible in this pixel (or
            // this sample, for antialiasing).  Once determined,
            // you'd set up a ShaderGlobals that contained the vital
            // information about that point, such as its location,
            // the normal there, the u and v coordinates on the
            // surface, the transformation of that object, and so
            // on.  
            //
            // This test app is not a real renderer, so we just
            // set it up rigged to look like we're rendering a single
            // quadrilateral that exactly fills the viewport, and that
            // setup is done in the following function call:
            setup_shaderglobals (shaderglobals, shadingsys, x, y);

            // Actually run the shader for this point
            if (entrylayer_index.empty()) {
                // Sole entry point for whole group, default behavior
                shadingsys->execute (*ctx, *shadergroup, shadeindex,
                                     shaderglobals, userdata_base_ptr,
                                     output_base_ptr);
            } else {
                // Explicit list of entries to call in order
                shadingsys->execute_init (*ctx, *shadergroup, shadeindex,
                                          shaderglobals, userdata_base_ptr,
                                          output_base_ptr);
                if (entrylayer_symbols.size()) {
                    for (size_t i = 0, e = entrylayer_symbols.size(); i < e; ++i)
                        shadingsys->execute_layer (*ctx, shadeindex,
                                                   shaderglobals,
                                                   userdata_base_ptr,
                                                   output_base_ptr,
                                                   entrylayer_symbols[i]);
                } else {
                    for (size_t i = 0, e = entrylayer_index.size(); i < e; ++i)
                        shadingsys->execute_layer (*ctx, shadeindex,
                                                   shaderglobals,
                                                   userdata_base_ptr,
                                                   output_base_ptr,
                                                   entrylayer_index[i]);
                }
                shadingsys->execute_cleanup (*ctx);
            }

            // Save all the designated outputs.  But only do so if we
            // are on the last iteration requested, so that if we are
            // doing a bunch of iterations for time trials, we only
            // including the output pixel copying once in the timing.
            if (save && (print_outputs || !output_placement))
                save_outputs (rend, shadingsys, ctx, x, y);
        }
    }

    // We're done shading with this context.
    shadingsys->release_context (ctx);
    shadingsys->destroy_thread_info(thread_info);
}



#if OSL_USE_BATCHED

// Set up the uniform portion of BatchedShaderGlobals fields
template<int WidthT>
static void
setup_uniform_shaderglobals (BatchedShaderGlobals<WidthT> &bsg, ShadingSystem *shadingsys)
{
    auto &usg = bsg.uniform;

    // Just zero the whole thing out to start
    memset(&usg, 0, sizeof(UniformShaderGlobals));

    // In our SimpleRenderer, the "renderstate" itself just a pointer to
    // the ShaderGlobals.
    usg.renderstate = &bsg;

    // Just make it look like all shades are the result of 'raytype' rays.
    usg.raytype = shadingsys->raytype_bit (ustring (raytype));;


    // For this problem we will treat several varying members of
    // the BatchedShaderGlobals as uniform values.  We can pass to the
    // Block's of varying data to assign_all(proxy,value) to populate all varying
    // entries with a uniform value;
    auto & vsg = bsg.varying;
    using OSL::assign_all;

    // Set "shader" space to be Mshad.  In a real renderer, this may be
    // different for each shader group.
    assign_all(vsg.shader2common, OSL::TransformationPtr (&Mshad));

    // Set "object" space to be Mobj.  In a real renderer, this may be
    // different for each object.
    assign_all(vsg.object2common, OSL::TransformationPtr (&Mobj));

    // Set up u,v to vary across the "patch", and also their derivatives.
    // Note that since u & x, and v & y are aligned, we only need to set
    // values for dudx and dvdy, we can set dvdx and dudy to 0.
    if (pixelcenters) {
        // Our patch is like an "image" with shading samples at the
        // centers of each pixel.
        if (false == vary_udxdy) {
            assign_all(vsg.dudx, uscale / xres);
            assign_all(vsg.dudy, 0.0f);
        }
        if (false == vary_vdxdy) {
            assign_all(vsg.dvdx, 0.0f);
            assign_all(vsg.dvdy, vscale / yres);
        }
    } else {
        // Our patch is like a Reyes grid of points, with the border
        // samples being exactly on u,v == 0 or 1.
        if (false == vary_udxdy) {
            assign_all(vsg.dudx, uscale / std::max (1, xres-1));
            assign_all(vsg.dudy, 0.0f);
        }
        if (false == vary_vdxdy) {
            assign_all(vsg.dvdx, 0.0f);
            assign_all(vsg.dvdy, vscale / std::max (1, yres-1));
        }
    }

    // Assume that position P is simply (u,v,1), that makes the patch lie
    // on [0,1] at z=1.
    // Derivatives with respect to x,y
    if (false == vary_Pdxdy) {
        assign_all(vsg.dPdx, Vec3 (vsg.dudx[0], vsg.dudy[0], 0.0f));
        assign_all(vsg.dPdy, Vec3 (vsg.dvdx[0], vsg.dvdy[0], 0.0f));
    }
    assign_all(vsg.dPdz, Vec3 (0.0f, 0.0f, 0.0f));  // just use 0 for volume tangent
    // Tangents of P with respect to surface u,v
    assign_all(vsg.dPdu, Vec3 (1.0f, 0.0f, 0.0f));
    assign_all(vsg.dPdv, Vec3 (0.0f, 1.0f, 0.0f));

    assign_all(vsg.I, Vec3 (0, 0, 0));
    assign_all(vsg.dIdx, Vec3 (0, 0, 0));
    assign_all(vsg.dIdy, Vec3 (0, 0, 0));

    // That also implies that our normal points to (0,0,1)
    assign_all(vsg.N, Vec3 (0, 0, 1));
    assign_all(vsg.Ng, Vec3 (0, 0, 1));

    assign_all(vsg.time, 0.0f);
    assign_all(vsg.dtime, 0.0f);
    assign_all(vsg.dPdtime, Vec3 (0, 0, 0));

    assign_all(vsg.Ps, Vec3 (0, 0, 0));
    assign_all(vsg.dPsdx, Vec3 (0, 0, 0));
    assign_all(vsg.dPsdy, Vec3 (0, 0, 0));

    // Set the surface area of the patch to 1 (which it is).  This is
    // only used for light shaders that call the surfacearea() function.
    assign_all(vsg.surfacearea, 1.0f);

    assign_all(vsg.flipHandedness, 0);
    assign_all(vsg.backfacing, 0);
}

template <int WidthT>
static inline void
setup_varying_shaderglobals (int lane, BatchedShaderGlobals<WidthT> & bsg, ShadingSystem *shadingsys,
                     int x, int y)
{
    auto & vsg = bsg.varying;

    // Set up u,v to vary across the "patch", and also their derivatives.
    // Note that since u & x, and v & y are aligned, we only need to set
    // values for dudx and dvdy, we can use the memset above to have set
    // dvdx and dudy to 0.
    float u;
    float v;
    if (pixelcenters) {
        // Our patch is like an "image" with shading samples at the
        // centers of each pixel.
        u = uscale * (float)(x+0.5f) / xres + uoffset;
        v = vscale * (float)(y+0.5f) / yres + voffset;
    } else {
        // Our patch is like a Reyes grid of points, with the border
        // samples being exactly on u,v == 0 or 1.
        u = uscale * ((xres == 1) ? 0.5f : (float) x / (xres - 1)) + uoffset;
        v = vscale * ((yres == 1) ? 0.5f : (float) y / (yres - 1)) + voffset;
    }

    vsg.u[lane] = u;
    vsg.v[lane] = v;
    if (vary_udxdy) {
        vsg.dudx[lane] = 1.0f - u;
        vsg.dudy[lane] = u;
    }
    if (vary_vdxdy) {
        vsg.dvdx[lane] = 1.0f - v;
        vsg.dvdy[lane] = v;
    }

    // Assume that position P is simply (u,v,1), that makes the patch lie
    // on [0,1] at z=1.
    vsg.P[lane] = Vec3 (u, v, 1.0f);
    if (vary_Pdxdy) {
        vsg.dPdx[lane] = Vec3 (1.0f - u, 1.0f - v, u*0.5);
        vsg.dPdy[lane] = Vec3 (1.0f - v, 1.0f - u, v*0.5);
    }
}




template <int WidthT>
void OSL_NOINLINE
batched_shade_region (SimpleRenderer *rend, ShaderGroup *shadergroup, OIIO::ROI roi, bool save);

template <int WidthT>
void
batched_shade_region (SimpleRenderer *rend, ShaderGroup *shadergroup, OIIO::ROI roi, bool save)
{
    // Request an OSL::PerThreadInfo for this thread.
    OSL::PerThreadInfo *thread_info = shadingsys->create_thread_info();

    // Request a shading context so that we can execute the shader.
    // We could get_context/release_constext for each shading point,
    // but to save overhead, it's more efficient to reuse a context
    // within a thread.
    ShadingContext *ctx = shadingsys->get_context (thread_info);

    // Set up shader globals and a little test grid of points to shade.
    BatchedShaderGlobals<WidthT> sgBatch;
    setup_uniform_shaderglobals (sgBatch, shadingsys);

    // std::cout << "shading roi y(" << roi.ybegin << ", " << roi.yend << ")";
    // std::cout << " x(" << roi.xbegin << ", " << roi.xend << ")" << std::endl;

    int rwidth = roi.width();
    int rheight = roi.height();
    int nhits = rwidth*rheight;

    int oHitIndex = 0;
    while (oHitIndex < nhits) {
        int bx[WidthT];
        int by[WidthT];
        int batchSize = std::min(WidthT, nhits-oHitIndex);

        // TODO: vectorize this loop
        for(int bi=0; bi < batchSize; ++bi) {
            int lHitIndex = oHitIndex + bi;
            // A real renderer would use the hit index to access data to populate shader globals
            int lx = lHitIndex%rwidth;
            int ly = lHitIndex/rwidth;
            int rx = roi.xbegin + lx;
            int ry = roi.ybegin + ly;
            setup_varying_shaderglobals (bi, sgBatch, shadingsys, rx, ry);
            // Remember the pixel x & y values to store the outputs after shading
            bx[bi] = rx;
            by[bi] = ry;
        }

        // Actually run the shader for this point
        if (entrylayer_index.empty()) {
            // Sole entry point for whole group, default behavior
            shadingsys->batched<WidthT>().execute(*ctx, *shadergroup, batchSize, sgBatch);
        } else {
            // Explicit list of entries to call in order
            shadingsys->batched<WidthT>().execute_init (*ctx, *shadergroup, batchSize, sgBatch);
            if (entrylayer_symbols.size()) {
                for (size_t i = 0, e = entrylayer_symbols.size(); i < e; ++i)
                    shadingsys->batched<WidthT>().execute_layer (*ctx, batchSize, sgBatch, entrylayer_symbols[i]);
            } else {
                for (size_t i = 0, e = entrylayer_index.size(); i < e; ++i)
                    shadingsys->batched<WidthT>().execute_layer (*ctx, batchSize, sgBatch, entrylayer_index[i]);
            }
            shadingsys->execute_cleanup (*ctx);
        }

        if (save)
        {
            batched_save_outputs<WidthT>(rend, shadingsys, ctx, shadergroup, batchSize, bx, by);
        }

        oHitIndex += batchSize;
    }

    // We're done shading with this context.
    shadingsys->release_context (ctx);
    shadingsys->destroy_thread_info(thread_info);
}
#endif

static void synchio() {
    // Synch all writes to stdout & stderr now (mostly for Windows)
    std::cout.flush();
    std::cerr.flush();
    fflush(stdout);
    fflush(stderr);
}

extern "C" OSL_DLL_EXPORT int
test_shade (int argc, const char *argv[])
{
    OIIO::Timer timer;

    // Get the command line arguments.  Those that set up the shader
    // instances are queued up in shader_setup_args for later handling.
    getargs (argc, argv);

    // For testing purposes, allow user to set global locale
    if (localename.size()) {
        std::locale::global (std::locale(localename.c_str()));
        if (debug1 || verbose)
            printf("testshade: locale '%s', floats look like: %g\n",
                   localename.c_str(), 3.5);
    }

    SimpleRenderer *rend = nullptr;
#ifdef OSL_USE_OPTIX
    if (use_optix)
        rend = new OptixGridRenderer;
    else
#endif
        rend = new SimpleRenderer;

    // Other renderer and global options
    if (debug1 || verbose)
        rend->errhandler().verbosity (ErrorHandler::VERBOSE);
    rend->attribute("saveptx", (int)saveptx);

    // Hand the userdata options from the command line over to the renderer
    rend->userdata.merge(userdata);

    // Request a TextureSystem (by default it will be the global shared
    // one). This isn't strictly necessary, if you pass nullptr to
    // ShadingSystem ctr, it will ask for the shared one internally.
    TextureSystem *texturesys = TextureSystem::create();

    // Create a new shading system.  We pass it the RendererServices
    // object that services callbacks from the shading system, the
    // TextureSystem (note: passing nullptr just makes the ShadingSystem
    // make its own TS), and an error handler.
    shadingsys = new ShadingSystem (rend, texturesys, &errhandler);
    rend->init_shadingsys (shadingsys);

    // Register the layout of all closures known to this renderer
    // Any closure used by the shader which is not registered, or
    // registered with a different number of arguments will lead
    // to a runtime error.
    register_closures(shadingsys);

    // Remember that each shader parameter may optionally have a
    // metadata hint [[int lockgeom=...]], where 0 indicates that the
    // parameter may be overridden by the geometry itself, for example
    // with data interpolated from the mesh vertices, and a value of 1
    // means that it is "locked" with respect to the geometry (i.e. it
    // will not be overridden with interpolated or
    // per-geometric-primitive data).
    // 
    // In order to most fully optimize the shader, we typically want any
    // shader parameter not explicitly specified to default to being
    // locked (i.e. no per-geometry override):
    shadingsys->attribute("lockgeom", 1);

    // Now we declare our shader.
    // 
    // Each material in the scene is comprised of a "shader group."
    // Each group is comprised of one or more "layers" (a.k.a. shader
    // instances) with possible connections from outputs of
    // upstream/early layers into the inputs of downstream/later layers.
    // A shader instance is the combination of a reference to a shader
    // master and its parameter values that may override the defaults in
    // the shader source and may be particular to this instance (versus
    // all the other instances of the same shader).
    // 
    // A shader group declaration typically looks like this:
    //
    //   ShaderGroupRef group = ss->ShaderGroupBegin ();
    //   ss->Parameter (*group, "paramname", TypeDesc paramtype, void *value);
    //      ... and so on for all the other parameters of...
    //   ss->Shader (*group, "shadertype", "shadername", "layername");
    //      The Shader() call creates a new instance, which gets
    //      all the pending Parameter() values made right before it.
    //   ... and other shader instances in this group, interspersed with...
    //   ss->ConnectShaders (*group, "layer1", "param1", "layer2", "param2");
    //   ... and other connections ...
    //   ss->ShaderGroupEnd (*group);
    // 
    // It looks so simple, and it really is, except that the way this
    // testshade program works is that all the Parameter() and Shader()
    // calls are done inside getargs(), as it walks through the command
    // line arguments, whereas the connections accumulate and have
    // to be processed at the end.  Bear with us.
    
    // Start the shader group and grab a reference to it.
    shadergroup = shadingsys->ShaderGroupBegin (groupname);

    // Revisit the command line arguments that we stashed to set up the
    // shader itself.
    process_shader_setup_args ((int)shader_setup_args.size(),
                               shader_setup_args.data());
    if (params.size()) {
        std::cerr << "ERROR: Pending parameters without a shader:";
        for (auto&& pv : params)
            std::cerr << " " << pv.name();
        std::cerr << "\n";
        std::cerr << "Did you mistakenly put --param after the shader declaration?\n";
        return EXIT_FAILURE;
    }

    if (! shadergroup) {
        std::cerr << "ERROR: Invalid shader group. Exiting testshade.\n";
        return EXIT_FAILURE;
    }

    // Set shading sys options again, in case late-encountered command line
    // options change their values.
    set_shadingsys_options ();

    if (groupname.size())
        shadingsys->attribute (shadergroup.get(), "groupname", groupname);

    // Now set up the connections
    for (size_t i = 0;  i < connections.size();  i += 4) {
        if (i+3 < connections.size()) {
            std::cout << "Connect " 
                      << connections[i] << "." << connections[i+1]
                      << " to " << connections[i+2] << "." << connections[i+3]
                      << "\n";
            synchio();
            bool ok = shadingsys->ConnectShaders (*shadergroup,
                                                  connections[i],
                                                  connections[i+1],
                                                  connections[i+2],
                                                  connections[i+3]);
            if (!ok) {
                return EXIT_FAILURE;
            }
        }
    }

    // End the group
    shadingsys->ShaderGroupEnd (*shadergroup);

    if (verbose || do_oslquery) {
        std::string pickle;
        shadingsys->getattribute (shadergroup.get(), "pickle", pickle);
        std::cout << "Shader group:\n---\n" << pickle << "\n---\n";
        std::cout << "\n";
        ustring groupname;
        shadingsys->getattribute (shadergroup.get(), "groupname", groupname);
        std::cout << "Shader group \"" << groupname << "\" layers are:\n";
        int num_layers = 0;
        shadingsys->getattribute (shadergroup.get(), "num_layers", num_layers);
        if (num_layers > 0) {
            std::vector<const char *> layers (size_t(num_layers), NULL);
            shadingsys->getattribute (shadergroup.get(), "layer_names",
                                      TypeDesc(TypeDesc::STRING, num_layers),
                                      &layers[0]);
            for (int i = 0; i < num_layers; ++i) {
                std::cout << "    " << (layers[i] ? layers[i] : "<unnamed>") << "\n";
                if (do_oslquery) {
                    OSLQuery q = shadingsys->oslquery(*shadergroup, i);
                    for (size_t p = 0;  p < q.nparams(); ++p) {
                        const OSLQuery::Parameter *param = q.getparam(p);
                        std::cout << "\t" << (param->isoutput ? "output "  : "")
                                  << param->type << ' ' << param->name << "\n";
                    }
                }
            }
        }
        std::cout << "\n";
    }
    if (archivegroup.size())
        shadingsys->archive_shadergroup (shadergroup.get(), archivegroup);

    if (outputfiles.size())
        std::cout << "\n";

    rend->shaders().push_back (shadergroup);

    // Set up the named transformations, including shader and object.
    // For this test application, we just do this statically; in a real
    // renderer, the global named space (like "myspace") would probably
    // be static, but shader and object spaces may be different for each
    // object.
    setup_transformations (*rend, Mshad, Mobj);

#ifdef OSL_USE_OPTIX
#if (OPTIX_VERSION >= 70000)
    if (use_optix)
        reinterpret_cast<OptixGridRenderer *> (rend)->synch_attributes();
#endif
#endif

    // Set up the image outputs requested on the command line
    setup_output_images (rend, shadingsys, shadergroup);

    if (debug1)
        test_group_attributes (shadergroup.get());

    if (num_threads < 1)
        num_threads = OIIO::Sysutil::hardware_concurrency();

    // We need to set the global attribute so any helper functions
    // respect our thread count, especially if we wanted only 1
    // thread, we want to avoid spinning up a thread pool or
    // OS overhead of destroying threads (like clearing virtual
    // memory pages they occupied)
    OIIO::attribute("threads",num_threads);

    synchio();

    rend->prepare_render ();

    double setuptime = timer.lap ();

    if (warmup)
        rend->warmup();
    double warmuptime = timer.lap ();

    // Allow a settable number of iterations to "render" the whole image,
    // which is useful for time trials of things that would be too quick
    // to accurately time for a single iteration
    for (int iter = 0;  iter < iters;  ++iter) {
        OIIO::ROI roi (0, xres, 0, yres);

        if (use_optix) {
            rend->render (xres, yres);
        } else if (use_shade_image) {
            // TODO: do we need a batched option/version of shade_image?
            OSL::shade_image (*shadingsys, *shadergroup, NULL,
                              *rend->outputbuf(0), outputvarnames,
                              pixelcenters ? ShadePixelCenters : ShadePixelGrid,
                              roi, num_threads);
        } else {
            bool save = (iter == (iters-1));   // save on last iteration
#if 0
            shade_region (rend, shadergroup.get(), roi, save);
#else
#if OSL_USE_BATCHED
            if (batched) {
                if (batch_size == 16) {
                    OIIO::ImageBufAlgo::parallel_image (roi, num_threads,
                        [&](OIIO::ROI sub_roi)->void {
                            batched_shade_region<16> (rend, shadergroup.get(), sub_roi, save);
                        });
                } else {
                    ASSERT((batch_size == 8) && "Unsupport batch size");
                    OIIO::ImageBufAlgo::parallel_image (roi, num_threads,
                        [&](OIIO::ROI sub_roi)->void {
                            batched_shade_region<8> (rend, shadergroup.get(), sub_roi, save);
                        });
                }
            } else
#endif
            {
                OIIO::ImageBufAlgo::parallel_image (roi, num_threads,
                    [&](OIIO::ROI sub_roi)->void {
                        shade_region (rend, shadergroup.get(), sub_roi, save);
                    });
            }
#endif
        }

        // If any reparam was requested, do it now
        if (reparams.size() && reparam_layer.size() && (iter + 1 < iters)) {
            for (size_t p = 0;  p < reparams.size();  ++p) {
                const ParamValue &pv (reparams[p]);
                shadingsys->ReParameter (*shadergroup, reparam_layer.c_str(),
                                         pv.name().c_str(), pv.type(),
                                         pv.data());
            }
        }
    }
    double runtime = timer.lap();

    // This awkward condition preserves an output oddity from long ago,
    // eliminating the need to update hundreds of ref outputs.
    if (outputfiles.size() == 1 && outputfiles[0] == "null")
        std::cout << "\n";

    // Write the output images to disk
    rend->finalize_pixel_buffer ();
    for (size_t i = 0;  i < rend->noutputs();  ++i) {
        if (print_outputs || outputfiles[i] == "null")
            continue;  // don't write an image file
        if (OIIO::ImageBuf* outputimg = rend->outputbuf(i)) {
            std::string filename = outputimg->name();
            TypeDesc datatype = outputimg->spec().format;
            if (dataformatname == "uint8")
                datatype = TypeDesc::UINT8;
            else if (dataformatname == "half")
                datatype = TypeDesc::HALF;
            else if (dataformatname == "float")
                datatype = TypeDesc::FLOAT;

            // JPEG, GIF, and PNG images should be automatically saved
            // as sRGB because they are almost certainly supposed to
            // be displayed on web pages.
            using namespace OIIO;
            if (Strutil::iends_with (filename, ".jpg") ||
                Strutil::iends_with (filename, ".jpeg") ||
                Strutil::iends_with (filename, ".gif") ||
                Strutil::iends_with (filename, ".png")) {
                ImageBuf ccbuf = ImageBufAlgo::colorconvert (*outputimg,
                                            "linear", "sRGB");
                ccbuf.write (filename, datatype);
            } else {
                outputimg->write (filename, datatype);
            }
        }
    }

    // Print some debugging info
    if (debug1 || runstats || profile) {
        double writetime = timer.lap();
        std::cout << "\n";
        std::cout << "Setup : " << OIIO::Strutil::timeintervalformat (setuptime,4) << "\n";
        std::cout << "Warmup: " << OIIO::Strutil::timeintervalformat (warmuptime,4) << "\n";
        std::cout << "Run   : " << OIIO::Strutil::timeintervalformat (runtime,4) << "\n";
        std::cout << "Write : " << OIIO::Strutil::timeintervalformat (writetime,4) << "\n";
        std::cout << "\n";
        std::cout << shadingsys->getstats (5) << "\n";
        OIIO::TextureSystem *texturesys = shadingsys->texturesys();
        if (texturesys)
            std::cout << texturesys->getstats (5) << "\n";
        std::cout << ustring::getstats() << "\n";
    }

    // Give the renderer a chance to do initial cleanup while everything is still alive
    rend->clear();

    // We're done with the shading system now, destroy it
    shadergroup.reset ();  // Must release this before destroying shadingsys

    delete shadingsys;
    int retcode = EXIT_SUCCESS;

    // Double check that there were no uncaught errors in the texture
    // system and image cache.
    std::string err = texturesys->geterror();
    if (!err.empty()) {
        std::cout << "ERRORS left in TextureSystem:\n" << err << "\n";
        retcode = EXIT_FAILURE;
    }
    OIIO::ImageCache *ic = texturesys->imagecache();
    err = ic ? ic->geterror() : std::string();
    if (!err.empty()) {
        std::cout << "ERRORS left in ImageCache:\n" << err << "\n";
        retcode = EXIT_FAILURE;
    }

    delete rend;

    return retcode;
}
