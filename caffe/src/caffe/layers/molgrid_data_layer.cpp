#include <fstream>  // NOLINT(readability/streams)
#include <iostream>  // NOLINT(readability/streams)
#include <string>
#include <utility>
#include <vector>

#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/algorithm/string.hpp>

#include "caffe/data_transformer.hpp"
#include "caffe/layers/base_data_layer.hpp"
#include "caffe/layers/molgrid_data_layer.hpp"
#include "caffe/util/benchmark.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/math_functions.hpp"

#include <openbabel/mol.h>
#include <openbabel/obconversion.h>
#include <openbabel/obiter.h>
#include <boost/timer/timer.hpp>


//allocate and initialize atom type data
namespace smina_atom_type
{
  info data[NumTypes] = {{},};

  struct atom_data_initializer {
    atom_data_initializer() {
      for(size_t i = 0u; i < smina_atom_type::NumTypes; ++i)
        smina_atom_type::data[i] = smina_atom_type::default_data[i];
    }
  };

  atom_data_initializer initialize_defaults;
}

namespace caffe {

template <typename Dtype>
MolGridDataLayer<Dtype>::~MolGridDataLayer<Dtype>() {
  //this->StopInternalThread();

  if(gpu_gridatoms) {
    cudaFree(gpu_gridatoms);
    gpu_gridatoms = NULL;
  }
  if(gpu_gridwhich) {
    cudaFree(gpu_gridwhich);
    gpu_gridwhich = NULL;
  }

  if(data) delete data;
  if(data2) delete data2;
}

template <typename Dtype>
MolGridDataLayer<Dtype>::example::example(MolGridDataLayer<Dtype>::string_cache& cache, string line, const MolGridDataParameter& param)
  : label(0), affinity(0.0), rmsd(0.0), affinity_weight(1.0)
{
  stringstream stream(line);
  string tmp;

  bool hasaffinity = param.has_affinity();
  bool hasrmsd = param.has_rmsd();
  unsigned numposes = param.num_poses();
  double affinity_reweight_mean = param.affinity_reweight_mean();
  double affinity_reweight_std = param.affinity_reweight_std();
  double affinity_reweight_stdcut = param.affinity_reweight_stdcut();

  //first the label
  stream >> label;
  if(hasaffinity)
   stream >> affinity;
  if(hasrmsd)
   stream >> rmsd;
  //receptor
  stream >> tmp;
  CHECK(tmp.length() > 0) << "Empty receptor, missing affinity/rmsd? Line:\n" << line;
  receptor = cache.get(tmp);
  //ligand(s)

  for(unsigned i = 0; i < numposes; i++) {
    tmp.clear();
    stream >> tmp;
    CHECK(tmp.length() > 0) << "Empty ligand, missing affinity/rmsd? Line:\n" << line;
    ligands.push_back(cache.get(tmp));
  }

  if(affinity_reweight_stdcut > 0 && affinity != 0) {
    //weight the affinities inversely to a normal distribution
    double x = fabs(fabs(affinity)-affinity_reweight_mean);
    x = min(x,affinity_reweight_stdcut*affinity_reweight_std);
    x = x*x; //square, but no negative since we want the inverse
    affinity_weight = exp(x/(2.0*affinity_reweight_std*affinity_reweight_std));
  }

}


//for in-memory inputs, set the desired label (for gradient computation)
//note that zero affinity/rmsd means to ignore these
template<typename Dtype>
void MolGridDataLayer<Dtype>::setLabels(Dtype pose, Dtype affinity, Dtype rmsd)
{
  labels.clear();
  affinities.clear();
  rmsds.clear();
  labels.push_back(pose);
  affinities.push_back(affinity);
  rmsds.push_back(rmsd);
}


//the following really shouldn't be recalculated each evaluation (not including gradients)
template<typename Dtype>
void MolGridDataLayer<Dtype>::getReceptorAtoms(int batch_idx, vector<float4>& atoms)
{
  atoms.resize(0);
  CHECK_LT(batch_idx,batch_transform.size()) << "Incorrect batch size in getReceptorAtoms";
  mol_info& mol = batch_transform[batch_idx].mol;
  for (unsigned i = 0, n = mol.atoms.size(); i < n; ++i)
    if (mol.whichGrid[i] < numReceptorTypes)
      atoms.push_back(mol.atoms[i]);
}

template<typename Dtype>
void MolGridDataLayer<Dtype>::getLigandAtoms(int batch_idx, vector<float4>& atoms)
{
  atoms.resize(0);
  CHECK_LT(batch_idx,batch_transform.size()) << "Incorrect batch size in getLigandAtoms";
  mol_info& mol = batch_transform[batch_idx].mol;
  for (unsigned i = 0, n = mol.atoms.size(); i < n; ++i)
    if (mol.whichGrid[i] >= numReceptorTypes)
      atoms.push_back(mol.atoms[i]);
}

template<typename Dtype>
void MolGridDataLayer<Dtype>::getReceptorChannels(int batch_idx, vector<short>& whichGrid)
{
  whichGrid.resize(0);
  CHECK_LT(batch_idx,batch_transform.size()) << "Incorrect batch size in getReceptorChannels";
  mol_info& mol = batch_transform[batch_idx].mol;
  for (unsigned i = 0, n = mol.atoms.size(); i < n; ++i)
    if (mol.whichGrid[i] < numReceptorTypes)
      whichGrid.push_back(mol.whichGrid[i]);
}

template<typename Dtype>
void MolGridDataLayer<Dtype>::getLigandChannels(int batch_idx, vector<short>& whichGrid)
{
  whichGrid.resize(0);
  CHECK_LT(batch_idx,batch_transform.size()) << "Incorrect batch size in getLigandChannels";

  mol_info& mol = batch_transform[batch_idx].mol;
  for (unsigned i = 0, n = mol.atoms.size(); i < n; ++i)
    if (mol.whichGrid[i] >= numReceptorTypes)
      whichGrid.push_back(mol.whichGrid[i]);
}

template<typename Dtype>
void MolGridDataLayer<Dtype>::getReceptorGradient(int batch_idx, vector<float3>& gradient)
{
  gradient.resize(0);
  CHECK_LT(batch_idx,batch_transform.size()) << "Incorrect batch size in getReceptorGradient";
  CHECK(compute_atom_gradients) << "Gradients requested but not computed";
  mol_info& mol = batch_transform[batch_idx].mol;
  for (unsigned i = 0, n = mol.atoms.size(); i < n; ++i)
    if (mol.whichGrid[i] < numReceptorTypes)
    {
      gradient.push_back(mol.gradient[i]);
    }
}

/*
 * Compute the transformation gradient of a rigid receptor around the center.
 * The first three numbers are the translation.  The next are the torque.
 */
template<typename Dtype>
void MolGridDataLayer<Dtype>::getReceptorTransformationGradient(int batch_idx, vec& force, vec& torque)
{
  force = vec(0,0,0);
  torque = vec(0,0,0);

  CHECK(compute_atom_gradients) << "Gradients requested but not computed";
  CHECK_LT(batch_idx,batch_transform.size()) << "Incorrect batch size in getReceptorTransformationGradient";

  mol_info& mol = batch_transform[batch_idx].mol;

  CHECK(mol.center == mem_lig.center) << "Centers not equal; receptor transformation gradient only supported in-mem";


  for (unsigned i = 0, n = mol.atoms.size(); i < n; ++i)
  {
    if (mol.whichGrid[i] < numReceptorTypes)
    {
      float3 g = mol.gradient[i];
      float4 a = mol.atoms[i];
      vec v(g.x,g.y,g.z);
      vec pos(a.x,a.y,a.z);

      force += v;
      torque += cross_product(pos - mol.center, v);
    }
  }
}


template<typename Dtype>
void MolGridDataLayer<Dtype>::getMappedReceptorGradient(int batch_idx, unordered_map<string, float3>& gradient)
{
  CHECK(compute_atom_gradients) << "Gradients requested but not computed";
  CHECK_LT(batch_idx,batch_transform.size()) << "Incorrect batch size in getMappedReceptorGradient";

  mol_info& mol = batch_transform[batch_idx].mol;
  for (unsigned i = 0, n = mol.atoms.size(); i < n; ++i)
  {
    if (mol.whichGrid[i] < numReceptorTypes)
    {
        string xyz = xyz_to_string(mol.atoms[i].x, mol.atoms[i].y, mol.atoms[i].z);
        gradient[xyz] = mol.gradient[i];
    }
  }
}


template<typename Dtype>
void MolGridDataLayer<Dtype>::getLigandGradient(int batch_idx, vector<float3>& gradient)
{
  CHECK(compute_atom_gradients) << "Gradients requested but not computed";
  CHECK_LT(batch_idx,batch_transform.size()) << "Incorrect batch size in getLigandGradient";

  gradient.resize(0);
  mol_info& mol = batch_transform[batch_idx].mol;
  for (unsigned i = 0, n = mol.atoms.size(); i < n; ++i)
    if (mol.whichGrid[i] >= numReceptorTypes)
    {
      gradient.push_back(mol.gradient[i]);
    }
}

template<typename Dtype>
void MolGridDataLayer<Dtype>::getMappedLigandGradient(int batch_idx, unordered_map<string, float3>& gradient)
{
  CHECK(compute_atom_gradients) << "Gradients requested but not computed";
  CHECK_LT(batch_idx,batch_transform.size()) << "Incorrect batch size in getMappedLigandGradient";

  mol_info& mol = batch_transform[batch_idx].mol;
  for (unsigned i = 0, n = mol.atoms.size(); i < n; ++i)
  {
    if (mol.whichGrid[i] >= numReceptorTypes)
    {
        string xyz = xyz_to_string(mol.atoms[i].x, mol.atoms[i].y, mol.atoms[i].z);
        gradient[xyz] = mol.gradient[i];
    }
  }
}


//modify examples to remove any without both actives an inactives
//factored this into its own function due to the need to fully specialize setup below
template<typename Dtype>
void MolGridDataLayer<Dtype>::remove_missing_and_setup(vector<typename MolGridDataLayer<Dtype>::balanced_example_provider>& examples)
{
  vector<balanced_example_provider> tmp;
  for(unsigned i = 0, n = examples.size(); i < n; i++)
  {
    if(examples[i].num_actives() > 0 && examples[i].num_decoys() > 0) {
      //eliminate empty buckets
      tmp.push_back(examples[i]);
      tmp.back().setup();
    }
    else if(examples[i].num_actives() > 0)
    {
      example tmp;
      examples[i].next_active(tmp);
      LOG(INFO) << "Dropping receptor " << tmp.receptor << " with no decoys.";
    }
    else if(examples[i].num_decoys() > 0)
    {
      example tmp;
      examples[i].next_decoy(tmp);
      LOG(INFO) << "Dropping receptor " << tmp.receptor << " with no actives.";
    }
  }

  swap(examples,tmp);
}

//specialized version for balanced data that remove receptors without any actives or decoys
//annoyingly, have to specialize Dtype
template<>
template<>
void MolGridDataLayer<float>::receptor_stratified_example_provider<typename MolGridDataLayer<float>::balanced_example_provider, 2>::setup()
{
  currenti = 0; currentk = 0;
  remove_missing_and_setup(examples);
  //also shuffle receptors
  if(randomize) shuffle(examples.begin(), examples.end(), caffe::caffe_rng());
}

template<>
template<>
void MolGridDataLayer<double>::receptor_stratified_example_provider<typename MolGridDataLayer<double>::balanced_example_provider, 2>::setup()
{
  currenti = 0; currentk = 0;
  remove_missing_and_setup(examples);
  //also shuffle receptors
  if(randomize) shuffle(examples.begin(), examples.end(), caffe::caffe_rng());
}



//ensure gpu memory is of sufficient size
template <typename Dtype>
void MolGridDataLayer<Dtype>::allocateGPUMem(unsigned sz)
{
  if(sz > gpu_alloc_size) {
    //deallocate
    if(gpu_gridatoms) {
      cudaFree(gpu_gridatoms);
    }
    if(gpu_gridwhich) {
      cudaFree(gpu_gridwhich);
    }
    //allocate larger
    CUDA_CHECK(cudaMalloc(&gpu_gridatoms, sz*sizeof(float4)));
    CUDA_CHECK(cudaMalloc(&gpu_gridwhich, sz*sizeof(short)));
  }
}

//allocate and return an example provider to the specifications of the parm object
template <typename Dtype>
typename MolGridDataLayer<Dtype>::example_provider* MolGridDataLayer<Dtype>::create_example_data(const MolGridDataParameter& parm)
{
  bool balanced  = parm.balanced();
  bool strat_receptor  = parm.stratify_receptor();
  bool strat_aff = parm.stratify_affinity_max() != parm.stratify_affinity_min();

  //strat_aff > strat_receptor > balanced
  if(strat_aff)
  {
    if(strat_receptor)
    {
      if(balanced) // sample 2 from each receptor
      {
        return new affinity_stratified_example_provider<receptor_stratified_example_provider<balanced_example_provider, 2> >(parm);
      }
      else //sample 1 from each receptor
      {
        return new affinity_stratified_example_provider<receptor_stratified_example_provider<uniform_example_provider, 1> >(parm);
      }
    }
    else
    {
      if(balanced)
      {
        return new affinity_stratified_example_provider<balanced_example_provider>(parm);
      }
      else //sample 1 from each receptor
      {
        return new affinity_stratified_example_provider<uniform_example_provider>(parm);
      }
    }
  }
  else if(strat_receptor)
  {
    if(balanced) // sample 2 from each receptor
    {
      return new receptor_stratified_example_provider<balanced_example_provider, 2>(parm);
    }
    else //sample 1 from each receptor
    {
      return new receptor_stratified_example_provider<uniform_example_provider, 1>(parm);
    }
  }
  else if(balanced)
  {
    return new balanced_example_provider(parm);
  }
  else
  {
    return new uniform_example_provider(parm);
  }
}

//make sure can append to root_folder path
static string sanitize_path(const string& p)
{
  //make sure root folder(s) have trailing slash
  if (p.length() > 0 && p[p.length()-1] != '/')
    return p + "/";
  else
    return p;
}

//fill in training examples
template <typename Dtype>
void MolGridDataLayer<Dtype>::populate_data(const string& root_folder, const string& source,
    MolGridDataLayer<Dtype>::example_provider* data, bool hasaffinity, bool hasrmsd)
{
  LOG(INFO) << "Opening file " << source;
  std::ifstream infile(source.c_str());
  CHECK((bool)infile) << "Could not open " << source;
  string line;
  while (getline(infile, line))
  {
    example ex(scache, line, this->layer_param_.molgrid_data_param());
    data->add(ex);
  }
  CHECK_GT(data->size(),0) << "No examples provided in source: " << source;

  data->setup(); //done adding

}

//read in structure input and atom type maps
template <typename Dtype>
void MolGridDataLayer<Dtype>::DataLayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {

  const MolGridDataParameter& param = this->layer_param_.molgrid_data_param();
  bool duplicate = param.duplicate_poses();

  root_folder = param.root_folder();
  num_rotations = param.rotate();
  inmem = param.inmemory();
  dimension = param.dimension();
  resolution = param.resolution();
  binary = param.binary_occupancy();
  bool spherize = param.spherical_mask();
  randtranslate = param.random_translate();
  randrotate = param.random_rotation();
  ligpeturb = param.peturb_ligand();
  ligpeturb_translate = param.peturb_ligand_translate();
  ligpeturb_rotate = param.peturb_ligand_rotate();
  jitter = param.jitter();
  ignore_ligand = param.ignore_ligand();
  radiusmultiple = param.radius_multiple();
  fixedradius = param.fixed_radius();
  use_covalent_radius = param.use_covalent_radius();
  bool hasaffinity = param.has_affinity();
  bool hasrmsd = param.has_rmsd();
  data_ratio = param.source_ratio();
  root_folder2 = param.root_folder2();
  numposes = param.num_poses();
  batch_rotate = param.batch_rotate();
  batch_rotate_yaw = param.batch_rotate_yaw();
  batch_rotate_roll = param.batch_rotate_roll();
  batch_rotate_pitch = param.batch_rotate_pitch();

  if(binary) radiusmultiple = 1.0;
  CHECK_LE(fabs(remainder(dimension,resolution)), 0.001) << "Resolution does not evenly divide dimension.";

  gmaker.initialize(resolution, dimension, radiusmultiple, binary, spherize);

  dim = round(dimension/resolution)+1; //number of grid points on a side
  numgridpoints = dim*dim*dim;
  if(numgridpoints % 512 != 0)
    LOG(INFO) << "Total number of grid points (" << numgridpoints << ") is not evenly divisible by 512.";

  //shape must come from parameters
  int batch_size = param.batch_size();

  if(!inmem)
  {

    const string& source = param.source();
    const string& source2 = param.source2();
    root_folder = sanitize_path(param.root_folder());
    root_folder2 = param.root_folder2();
    if(root_folder2.length() > 0) root_folder2 = sanitize_path(root_folder2);
    else root_folder2 = root_folder; //fall back on first

    CHECK_GT(source.length(), 0) << "No data source file provided";

    // Read source file(s) with labels and structures,
    // each line is label [affinity] [rmsd] receptor_file ligand_file
    data = create_example_data(param);
    populate_data(root_folder, source, data, hasaffinity, hasrmsd);

    if(source2.length() > 0)
    {
      CHECK_GE(data_ratio, 0) << "Must provide non-negative ratio for two data sources";
      data2 = create_example_data(param);
      populate_data(root_folder2, source2, data2, hasaffinity, hasrmsd);
    }

    LOG(INFO) << "Total examples: " << data->size() + (data2 ? data2->size() : 0);

    // Check if we would need to randomly skip a few data points
    if (param.rand_skip())
    {
      unsigned int skip = caffe_rng_rand() %  param.rand_skip();

      LOG(INFO) << "Skipping first " << skip << " data points from each source.";

      example dummy;
      for(unsigned i = 0; i < skip; i++) {
        data->next(dummy);
      }
      if(data2)
      {
        for(unsigned i = 0; i < skip; i++) {
          data2->next(dummy);
        }
      }
    }
  }
  else //in memory always batch size of 1
  {
    batch_size = 1;
  }

  //initialize atom type maps
  string recmapfile = param.recmap();  //these are file names
  string ligmapfile = param.ligmap();

  //these are the actual contents
  string recmapstr = param.mem_recmap();
  string ligmapstr = param.mem_ligmap();

  //can specify maps programatically
  if(recmapstr.size())
    numReceptorTypes = GridMaker::createMapFromString(recmapstr, rmap);
  else if(recmapfile.size() > 0)
    numReceptorTypes = GridMaker::createAtomTypeMap(recmapfile, rmap);
  else
    numReceptorTypes = GridMaker::createDefaultRecMap(rmap);


  if(ligmapstr.size())
    numLigandTypes = GridMaker::createMapFromString(ligmapstr, lmap);
  else if (ligmapfile.size() > 0)
    numLigandTypes = GridMaker::createAtomTypeMap(ligmapfile, lmap);
  else
    numLigandTypes = GridMaker::createDefaultLigMap(lmap);

  CHECK_GT(batch_size, 0) << "Positive batch size required";
  //keep track of atoms and transformations for each example in batch
  batch_transform.resize(batch_size);

  //if specified, preload all gninatype information
  string reccache = param.recmolcache();
  string ligcache = param.ligmolcache();

  if(reccache.size() > 0) {
    load_cache(reccache, rmap, 0, recmolcache);
  }

  if(ligcache.size() > 0) {
    load_cache(ligcache, rmap, numReceptorTypes, ligmolcache);
  }

  //setup shape of layer
  top_shape.clear();
  unsigned number_examples = batch_size;
  if(duplicate) number_examples = batch_size*numposes;
  top_shape.push_back(number_examples);
  
  numchannels = numReceptorTypes+numLigandTypes;
  if(!duplicate && numposes > 1) numchannels = numReceptorTypes+numposes*numLigandTypes;
  top_shape.push_back(numchannels);
  top_shape.push_back(dim);
  top_shape.push_back(dim);
  top_shape.push_back(dim);

  example_size = (numchannels)*numgridpoints;

  // Reshape prefetch_data and top[0] according to the batch_size.
  top[0]->Reshape(top_shape);

  // Reshape label, affinity, rmsds
  vector<int> label_shape(1, number_examples); // [batch_size]

  top[1]->Reshape(label_shape);

  if (hasaffinity)
  {
    top[2]->Reshape(label_shape);
    if (hasrmsd)
    {
      top[3]->Reshape(label_shape);
    }
  }
  else if(hasrmsd)
  {
    top[2]->Reshape(label_shape);
  }

  if(param.affinity_reweight_stdcut() > 0) {
    unsigned indx = top.size()-1;
    if(ligpeturb) indx--; //this is getting cumbersome
    top[indx]->Reshape(label_shape);
  }

  if(ligpeturb) {
    vector<int> peturbshape(2);
    peturbshape[0] = batch_size;
    peturbshape[1] = output_transform::size(); //trans+orient
    top.back()->Reshape(peturbshape);
  }
}

//return quaternion representing one of 24 distinct axial rotations
template <typename Dtype>
typename MolGridDataLayer<Dtype>::quaternion MolGridDataLayer<Dtype>::axial_quaternion()
{
  unsigned rot = current_rotation;
  qt ret;
  //first rotate to a face
  switch(rot%6) {
    case 0:
      ret = quaternion(1,0,0,0); //identity
      break;
    case 1: //rotate z 90
      ret = quaternion(sqrt(0.5),0,0,sqrt(0.5));
      break;
    case 2: //rotate z 180
      ret = quaternion(0,0,0,1);
      break;
    case 3: //rotate z 270 (-90)
      ret = quaternion(sqrt(0.5),0,0,-sqrt(0.5));
      break;
    case 4: //rotate y 90
      ret = quaternion(sqrt(0.5),0,sqrt(0.5),0);
      break;
    case 5: //rotate y -90
      ret = quaternion(sqrt(0.5),0,-sqrt(0.5),0);
      break;
  }

  //now four rotations around x axis
  rot /= 6;
  switch(rot%4) {
    case 0:
      break; //identity
    case 1: //90 degrees
      ret *= quaternion(sqrt(0.5),sqrt(0.5),0,0);
      break;
    case 2: //180
      ret *= quaternion(0,1,0,0);
      break;
    case 3: //270
      ret *= quaternion(sqrt(0.5),-sqrt(0.5),0,0);
      break;
  }
  return ret;
}

//add atom information to minfo, return true if atom actually added
template <typename Dtype>
bool MolGridDataLayer<Dtype>::add_to_minfo(const string& file, const vector<int>& atommap, unsigned mapoffset, smt t, float x, float y, float z,  mol_info& minfo)
{
  int index = atommap[t];
  if(index >= 0)
  {
    float4 ainfo;
    ainfo.x = x;
    ainfo.y = y;
    ainfo.z  = z;
    if(fixedradius <= 0)
      ainfo.w = use_covalent_radius ? covalent_radius(t) : xs_radius(t);
    else
      ainfo.w = fixedradius;

    float3 gradient(0,0,0);
    minfo.atoms.push_back(ainfo);
    minfo.whichGrid.push_back(index+mapoffset);
    minfo.gradient.push_back(gradient);
  }
  else
  {
    static bool madewarning = false;
    if(!madewarning) {
      LOG(WARNING) << "WARNING: Unknown atom type " << t << " in " << file << ".  This atom will be discarded.  Future warnings will be suppressed\n";
      madewarning = true;
    }
    return 0;
  }
  return 1;
}

//two shared caches for the whole program
template<>
MolGridDataLayer<float>::MolCache MolGridDataLayer<float>::recmolcache = MolGridDataLayer<float>::MolCache();
template<>
MolGridDataLayer<double>::MolCache MolGridDataLayer<double>::recmolcache =  MolGridDataLayer<double>::MolCache();

template<>
MolGridDataLayer<float>::MolCache MolGridDataLayer<float>::ligmolcache = MolGridDataLayer<float>::MolCache();
template<>
MolGridDataLayer<double>::MolCache MolGridDataLayer<double>::ligmolcache =  MolGridDataLayer<double>::MolCache();

//load custom formatted cache file of all gninatypes into specified molcache using specified mapping and offset
template <typename Dtype>
void MolGridDataLayer<Dtype>::load_cache(const string& file, const vector<int>& atommap, unsigned mapoffset, MolGridDataLayer<Dtype>::MolCache& molcache)
{
  //file shoudl be organized
  //name size (1byte)
  //name (string)
  //number of atoms (4bytes)
  //atoms (3 floats and an int of the type)

  char buffer[257] = {0,};
  struct info {
    float x,y,z;
    int type;
  } atom;

  string fullpath = file;
  if(file.size() > 0 && file[0] != '/')
    fullpath = root_folder + file; //prepend dataroot if not absolute
  ifstream in(fullpath.c_str());
  CHECK(in) << "Could not read " << fullpath;

  LOG(INFO) << "Loading from " << fullpath << " with cache at size " << molcache.size() << "\n";
  while(in && in.peek() != EOF)
  {
    char sz = 0;
    int natoms = 0;
    in.read(&sz, sizeof(char));
    in.read(buffer,sizeof(char)*sz);
    buffer[(int)sz] = 0; //null terminate
    string fname(buffer);

    in.read((char*)&natoms, sizeof(int));

    if(molcache.count(fname)) {
      static int warncnt = 0;

      if(warncnt == 0) {
        LOG(WARNING) << "File " << fname << " duplicated in provided cache " << file << ".  Future warnings are supressed.";
        warncnt++;
      }
    }

    mol_info& minfo = molcache[fname];
    minfo.atoms.clear();
    minfo.whichGrid.clear();
    minfo.gradient.clear();
    int cnt = 0;
    vec center(0,0,0);

    for(unsigned i = 0; i < natoms; i++)
    {
      in.read((char*)&atom, sizeof(atom));
      smt t = (smt)atom.type;

      if(add_to_minfo(fname, atommap, mapoffset, t, atom.x, atom.y, atom.z, minfo)) {
        cnt++;
        center += vec(atom.x,atom.y,atom.z);
      }
    }

    if(cnt == 0) {
      LOG(WARNING) << "WARNING: No atoms in " << file <<"\n";
      continue;
    }

    center /= cnt;
    minfo.center = center;
  }

  LOG(INFO) << "Done loading from " << fullpath << " with cache at size " << molcache.size() << std::endl;

}

template <typename Dtype>
void MolGridDataLayer<Dtype>::set_mol_info(const string& file, const vector<int>& atommap,
    unsigned mapoffset, mol_info& minfo)
{
  //read mol info from file
  //OpenBabel is SLOW, especially for the receptor, so we cache the result
  //if this gets too annoying, can add support for spawning a thread for openbabel
  //but since this gets amortized across many hits to the same example, not a high priority
  //if clear is true, set, otherwise add
  using namespace OpenBabel;

  minfo.atoms.clear();
  minfo.whichGrid.clear();
  minfo.gradient.clear();
  
  int cnt = 0;
  vec center(0,0,0);

  //also, implemented a custom gninatypes files to precalc this info
  if(boost::algorithm::ends_with(file,".gninatypes"))
  {
    struct info {
      float x,y,z;
      int type;
    } atom;

    ifstream in(file.c_str());
    CHECK(in) << "Could not read " << file;

    while(in.read((char*)&atom, sizeof(atom)))
    {
      smt t = (smt)atom.type;

      if(add_to_minfo(file, atommap, mapoffset, t, atom.x, atom.y, atom.z, minfo)) {
        cnt++;
        center += vec(atom.x,atom.y,atom.z);
      }
    }
  }
  else if(!boost::algorithm::ends_with(file,"none")) //reserved word
  {
    //read mol from file and set mol info (atom coords and grid positions)
    //types are mapped using atommap values plus offset
    OpenBabel::OBConversion conv;
    OBMol mol;
    CHECK(conv.ReadFile(&mol, file)) << "Could not read " << file;

    if(this->layer_param_.molgrid_data_param().addh()) {
      mol.AddHydrogens();
    }

    minfo.atoms.reserve(mol.NumHvyAtoms());
    minfo.whichGrid.reserve(mol.NumHvyAtoms());
    minfo.gradient.reserve(mol.NumHvyAtoms());

    FOR_ATOMS_OF_MOL(a, mol)
    {
      smt t = obatom_to_smina_type(*a);
      if(add_to_minfo(file, atommap, mapoffset, t, a->x(), a->y(), a->z(), minfo)) {
        cnt++;
        center += vec(a->x(), a->y(), a->z());
      }
    }
  }

  if(cnt == 0) {
    std::cerr << "WARNING: No atoms in " << file <<"\n";
  }
  else {
    center /= cnt;
  }
  minfo.center = center;

}

template <typename Dtype>
void MolGridDataLayer<Dtype>::set_grid_ex(Dtype *data, const MolGridDataLayer<Dtype>::example& ex,
    const string& root_folder, MolGridDataLayer<Dtype>::mol_transform& transform, unsigned pose, output_transform& peturb, bool gpu)
{
  //set grid values for example
  //cache atom info
  //pose specifies which ligand pose to use (relevant if num_poses > 1)
  //if it is negative, use them all (but with distinct channels
  //data should be positioned at the start of the example
  bool docache = this->layer_param_.molgrid_data_param().cache_structs();
  bool doall = false;
  if(pose < 0) {
      doall = true;
      pose = 0;
  }

  CHECK_LT(pose, ex.ligands.size()) << "Incorrect pose index";
  const char* ligand = ex.ligands[pose];

  if(docache)
  {
    if(recmolcache.count(ex.receptor) == 0)
    {
      set_mol_info(root_folder+ex.receptor, rmap, 0, recmolcache[ex.receptor]);
    }
    if(ligmolcache.count(ligand) == 0)
    {
      set_mol_info(root_folder+ligand, lmap, numReceptorTypes, ligmolcache[ligand]);
    }

    if(doall) {
      //make sure every ligand is in the cache, then aggregate
      mol_info lig(ligmolcache[ligand]);
      for(unsigned p = 1, np = ex.ligands.size(); p < np; p++) {
        ligand = ex.ligands[p];
        if(ligmolcache.count(ligand) == 0)
        {
          set_mol_info(root_folder+ligand, lmap, numReceptorTypes, ligmolcache[ligand]);
        }
        lig.append(ligmolcache[ligand],numLigandTypes*p);
      }
      set_grid_minfo(data, recmolcache[ex.receptor], lig, transform, peturb, gpu);
    } else {
        set_grid_minfo(data, recmolcache[ex.receptor], ligmolcache[ligand], transform, peturb, gpu);
    }
  }
  else
  {
    mol_info rec;
    mol_info lig;
    set_mol_info(root_folder+ex.receptor, rmap, 0, rec);
    set_mol_info(root_folder+ligand, lmap, numReceptorTypes, lig);
    if(doall) {
        for(unsigned p = 1, np = ex.ligands.size(); p < np; p++) {
          mol_info tmplig;
          set_mol_info(root_folder+ligand, lmap, numReceptorTypes+numLigandTypes*p, tmplig);
          lig.append(tmplig);
        }
    }    
    set_grid_minfo(data, rec, lig, transform, peturb, gpu);
  }
}


template <typename Dtype>
void MolGridDataLayer<Dtype>::set_grid_minfo(Dtype *data, const MolGridDataLayer<Dtype>::mol_info& recatoms,
    const MolGridDataLayer<Dtype>::mol_info& ligatoms,
    MolGridDataLayer<Dtype>::mol_transform& transform,
    output_transform& peturb, bool gpu)
    {
  bool fixcenter = this->layer_param_.molgrid_data_param().fix_center_to_origin();
  //set grid values from mol info
  //first clear transform from the previous batch
  rng_t* rng = caffe_rng();
  transform = mol_transform();
  mol_transform ligtrans;

  //figure out transformation
  //note there are currently two code paths - one where setAtoms performs the transformation
  //and one where the transformation is applied here; the first is faster, as it can be done
  //on the GPU, but the second let's us support "weird" transformation like peturbations
  //in the first case, the coordinates of the mol don't change; in the other they are mogrified
  //TODO: unify this - I think transformation should be treated separately from gridding
  //I don't think random rotation and backwards gradients are currently working
  if (!batch_rotate)
  {
    transform.Q = quaternion(1, 0, 0, 0);
  }

  if (current_rotation == 0 && !randrotate)
    transform.Q = quaternion(1, 0, 0, 0); //check real part to avoid mult

  if (randrotate)
  {
    transform.set_random_quaternion(rng);
  }

  if (randtranslate)
  {
    double radius = ligatoms.radius();
    //don't let ligand atoms translate out of sphere inscribed in box
    if (ignore_ligand) radius = 0;
    double maxtrans = max(dimension / 2.0 - radius, 0.0);
    transform.add_random_displacement(rng, min(randtranslate, maxtrans));
  }

  if (current_rotation > 0) {
    transform.Q *= axial_quaternion();
  }

  //include receptor and ligand atoms
  transform.mol.append(recatoms);
  //set rotation center to ligand center
  transform.mol.center = ligatoms.center;

  //GPU transformation will use Q and grid_center to apply transformation
  quaternion Q = transform.Q;
  vec grid_center(0, 0, 0);

  if(!fixcenter) {
  //center on ligand, offset by random translate
    transform.center += ligatoms.center;
    grid_center = transform.center;
  }

  //both ligand_peturbation and zero center need to apply mol transform here,
  //since they don't fit the current framework for GPU transformation
  //TODO move this into gridmaker.setAtoms, have it take separate receptor and ligand transformations (or have addAtoms)
  mol_info ligmol = ligatoms;
  if (fixcenter || ligpeturb) {
    transform.mol.apply_transform(transform); //mogrify the coordinates -- these are recatoms only, rotate around ligand center
    ligmol.apply_transform(transform); //modify ligand coordinates
    //Q is already applied
    Q = qt(1, 0, 0, 0);
    grid_center = vec(0,0,0);
  }

  //add ligatoms to transform.mol
  if (ligpeturb) {
    if (ligpeturb_rotate)
    {
      ligtrans.set_random_quaternion(rng);
    }
    else
    {
      ligtrans.Q = quaternion(1, 0, 0, 0); //identity
    }
    ligtrans.add_random_displacement(rng, ligpeturb_translate);
    ligmol.apply_transform(ligtrans); //peturb
    transform.mol.append(ligmol); //append

    //store the inverse transformation
    peturb.x = ligtrans.center[0];
    peturb.y = ligtrans.center[1];
    peturb.z = ligtrans.center[2];

    qt qinv = conj(ligtrans.Q) / norm(ligtrans.Q); //not Cayley, not euclidean norm - already squared
    peturb.set_from_quaternion(qinv);

    //set the center to the translated value
    transform.mol.center = ligmol.center + ligtrans.center;
  } else if (ignore_ligand) {
    //do nothing - ligand is only used to set center
  } else {
    transform.mol.append(ligmol);
  }

  //with fix_center, this should be zero
  gmaker.setCenter(grid_center[0], grid_center[1], grid_center[2]);

  if (transform.mol.atoms.size() == 0) {
    std::cerr << "ERROR: No atoms in molecule.  I can't deal with this.\n";
    exit(-1); //presumably you never actually want this and it results in a cuda error
  }
  if (jitter > 0) {
    //add small random displacement (in-place) to atoms
    for (unsigned i = 0, n = transform.mol.atoms.size(); i < n; i++) {
      float4& atom = transform.mol.atoms[i];
      float xdiff = jitter * (unit_sample(rng) * 2.0 - 1.0);
      atom.x += xdiff;
      float ydiff = jitter * (unit_sample(rng) * 2.0 - 1.0);
      atom.y += ydiff;
      float zdiff = jitter * (unit_sample(rng) * 2.0 - 1.0);
      atom.z += zdiff;
    }
  }

  //compute grid from atom info arrays
  if (gpu)
  {
    unsigned natoms = transform.mol.atoms.size();
    allocateGPUMem(natoms);
    CUDA_CHECK(
        cudaMemcpy(gpu_gridatoms, &transform.mol.atoms[0],
            natoms * sizeof(float4), cudaMemcpyHostToDevice));
    CUDA_CHECK(
        cudaMemcpy(gpu_gridwhich, &transform.mol.whichGrid[0],
            natoms * sizeof(short), cudaMemcpyHostToDevice));

    gmaker.setAtomsGPU<Dtype>(natoms, gpu_gridatoms, gpu_gridwhich, Q,
        numchannels, data);
  }
  else
  {
    Grids grids(data, boost::extents[numchannels][dim][dim][dim]);
    gmaker.setAtomsCPU(transform.mol.atoms, transform.mol.whichGrid, Q.boost(),
        grids);
  }
}


//return a string representation of the atom type(s) represented by index
//in map - this isn't particularly efficient, but is only for debug purposes
template <typename Dtype>
string MolGridDataLayer<Dtype>::getIndexName(const vector<int>& map, unsigned index) const
		{
	stringstream ret;
	stringstream altret;
	for (unsigned at = 0; at < smina_atom_type::NumTypes; at++)
	{
		if (map[at] == index)
		{
			ret << smina_type_to_string((smt) at);
			altret << "_" << at;
		}
	}

	if (ret.str().length() > 32) //there are limits on file name lengths
		return altret.str();
	else
		return ret.str();
}


//output a grid the file in dx format (for debug)
template<typename Dtype>
void MolGridDataLayer<Dtype>::outputDXGrid(std::ostream& out, Grids& grid, unsigned g, double scale) const
{
  unsigned n = dim;
  out.precision(5);
  setprecision(5);
  out << fixed;
  out << "object 1 class gridpositions counts " << n << " " << n << " " << " " << n << "\n";
  out << "origin";
  for (unsigned i = 0; i < 3; i++)
  {
    out << " " << mem_lig.center[i]-dimension/2.0;
  }
  out << "\n";
  out << "delta " << resolution << " 0 0\ndelta 0 " << resolution << " 0\ndelta 0 0 " << resolution << "\n";
  out << "object 2 class gridconnections counts " << n << " " << n << " " << " " << n << "\n";
  out << "object 3 class array type double rank 0 items [ " << n*n*n << "] data follows\n";
  //now coordinates - x,y,z
  out << scientific;
  out.precision(6);
  unsigned total = 0;
  for (unsigned i = 0; i < n; i++)
  {
    for (unsigned j = 0; j < n; j++)
    {
      for (unsigned k = 0; k < n; k++)
      {
        out << grid[g][i][j][k]*scale;
        total++;
        if(total % 3 == 0) out << "\n";
        else out << " ";
      }
    }
  }
}

//dump dx files for every atom type, with files names starting with prefix
//only does the very first grid for now
template<typename Dtype>
void MolGridDataLayer<Dtype>::dumpDiffDX(const std::string& prefix,
		Blob<Dtype>* top, double scale) const
{
	Grids grids(top->mutable_cpu_diff(),
			boost::extents[numReceptorTypes + numLigandTypes][dim][dim][dim]);
    CHECK_GT(mem_lig.atoms.size(),0) << "DX dump only works with in-memory ligand";
    CHECK_EQ(randrotate, false) << "DX dump requires no rotation";
    CHECK_LE(numposes, 1) << "DX dump requires numposes == 1";
	for (unsigned a = 0, na = numReceptorTypes; a < na; a++) {
		string name = getIndexName(rmap, a);
		string fname = prefix + "_rec_" + name + ".dx";
		ofstream out(fname.c_str());
		outputDXGrid(out, grids, a, scale);
	}
	for (unsigned a = 0, na = numLigandTypes; a < na; a++) {
			string name = getIndexName(lmap, a);
			string fname = prefix + "_lig_" + name + ".dx";
			ofstream out(fname.c_str());
			outputDXGrid(out, grids, numReceptorTypes+a, scale);
	}

}


template <typename Dtype>
void MolGridDataLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top)
{
  forward(bottom, top, false);
}


template <typename Dtype>
void MolGridDataLayer<Dtype>::forward(const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top, bool gpu)
{
  bool hasaffinity = this->layer_param_.molgrid_data_param().has_affinity();
  bool hasrmsd = this->layer_param_.molgrid_data_param().has_rmsd();
  bool hasweights = (this->layer_param_.molgrid_data_param().affinity_reweight_stdcut() > 0);
  bool duplicate = this->layer_param_.molgrid_data_param().duplicate_poses();
  int peturb_bins = this->layer_param_.molgrid_data_param().peturb_bins();
  double peturb_translate = this->layer_param_.molgrid_data_param().peturb_ligand_translate();

  Dtype *top_data = NULL;
  if(gpu)
    top_data = top[0]->mutable_gpu_data();
  else
    top_data = top[0]->mutable_cpu_data();

  perturbations.clear();
  unsigned div = 1;
  if(numposes > 1 && duplicate) div = numposes;
  unsigned batch_size = top_shape[0]/div;
  if(duplicate) CHECK_EQ(top_shape[0] % numposes,0) << "Batch size not multiple of numposes??";
  output_transform peturb;
  quaternion Q;

  //if in memory must be set programmatically
  if(inmem)
  {
    if(mem_rec.atoms.size() == 0) LOG(WARNING) << "Receptor not set in MolGridDataLayer";
    CHECK_GT(mem_lig.atoms.size(),0) << "Ligand not set in MolGridDataLayer";
    //memory is now available
    set_grid_minfo(top_data, mem_rec, mem_lig, batch_transform[0], peturb, gpu); //TODO how do we know what batch position?
    perturbations.push_back(peturb);

    if (num_rotations > 0) {
      current_rotation = (current_rotation+1)%num_rotations;
    }

    CHECK_GT(labels.size(),0) << "Did not set labels in memory based molgrid";

  }
  else
  {
    //clear batch labels
    labels.clear();
    affinities.clear();
    rmsds.clear();
    weights.clear();

    //percent of batch from first data source
    unsigned dataswitch = batch_size;
    if (data2)
      dataswitch = batch_size*data_ratio/(data_ratio+1);


    for (int batch_idx = 0; batch_idx < batch_size; ++batch_idx)
    {
      example ex;
      string *root;
      if (batch_idx < dataswitch)
      {
        data->next(ex);
        root = &root_folder;
      }
      else
      {
        data2->next(ex);
        root = &root_folder2;
      }

      if (batch_rotate)
      {
        double cy = cos(batch_rotate_yaw * 0.5 * batch_idx);
        double sy = sin(batch_rotate_yaw * 0.5 * batch_idx);
        double cr = cos(batch_rotate_roll * 0.5 * batch_idx);
        double sr = sin(batch_rotate_roll * 0.5 * batch_idx);
        double cp = cos(batch_rotate_pitch * 0.5 * batch_idx);
        double sp = sin(batch_rotate_pitch * 0.5 * batch_idx);
        batch_transform[batch_idx].Q = quaternion(cy*cr*cp + sy*sr*sp,
                                                  cy*sr*cp - sy*cr*sp,
                                                  cy*cr*sp + sy*sr*cp,
                                                  sy*cr*cp - cy*sr*sp);
      }

      if(!duplicate) {
        labels.push_back(ex.label);
        affinities.push_back(ex.affinity);
        rmsds.push_back(ex.rmsd);
        weights.push_back(ex.affinity_weight);
        int offset = batch_idx*example_size;
        set_grid_ex(top_data+offset, ex, *root, batch_transform[batch_idx], numposes > 1 ? -1 : 0, peturb, gpu);        
        perturbations.push_back(peturb); //peturb is set by grid_ex

      } else {
      	for(unsigned p = 0; p < numposes; p++) {
          labels.push_back(ex.label);
          affinities.push_back(ex.affinity);
          rmsds.push_back(ex.rmsd);
          weights.push_back(ex.affinity_weight);

          int offset = batch_idx*(example_size*numposes)+example_size*p;
          set_grid_ex(top_data+offset, ex, *root, batch_transform[batch_idx], p, peturb, gpu);
          perturbations.push_back(peturb);
        //NOTE: num_rotations not actually implemented!
        }
      }
      //NOTE: batch_transform contains transformation of last pose only - don't use unless numposes == 1

    }

  }

  unsigned weighti = top.size()-1-ligpeturb;
  unsigned rmsdi = 2+hasaffinity;

  if(gpu) {
    caffe_copy(labels.size(), &labels[0], top[1]->mutable_gpu_data());
    if(hasaffinity) {
      caffe_copy(affinities.size(), &affinities[0], top[2]->mutable_gpu_data());
    }
    if(hasrmsd) {
      caffe_copy(rmsds.size(), &rmsds[0], top[rmsdi]->mutable_gpu_data());
    }
    if(hasweights) {
      caffe_copy(weights.size(), &weights[0],top[weighti]->mutable_gpu_data());
    }
    if(ligpeturb) {
      //trusting struct layout is normal
      caffe_copy(perturbations.size()*perturbations[0].size(), (Dtype*)&perturbations[0], top.back()->mutable_gpu_data());
    }

  }
  else {
    caffe_copy(labels.size(), &labels[0], top[1]->mutable_cpu_data());
    if(hasaffinity) {
      caffe_copy(affinities.size(), &affinities[0], top[2]->mutable_cpu_data());
    }
    if(hasrmsd) {
      caffe_copy(rmsds.size(), &rmsds[0], top[rmsdi]->mutable_cpu_data());
    }
    if(hasweights) {
      caffe_copy(weights.size(), &weights[0],top[weighti]->mutable_cpu_data());
    }

    if(ligpeturb) {
      //trusting struct layout is normal
      caffe_copy(perturbations.size()*perturbations[0].size(), (Dtype*)&perturbations[0], top.back()->mutable_cpu_data());
    }
  }

  if(peturb_bins > 0) {
    //discretize
    for(unsigned i = 0, n = perturbations.size(); i < n; i++) {
      perturbations[i].discretize(peturb_translate, peturb_bins);
    }

  }

}


template <typename Dtype>
void MolGridDataLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top,
    const vector<bool>& propagate_down, const vector<Blob<Dtype>*>& bottom)
{
  backward(top, bottom, false);
}


/* backpropagates gradients onto atoms (note there is not actual bottom)
 * Only performed when compute_atom_gradients is true.
 */
template <typename Dtype>
void MolGridDataLayer<Dtype>::backward(const vector<Blob<Dtype>*>& top, const vector<Blob<Dtype>*>& bottom, bool gpu)
{
  //propagate gradient grid onto atom positions
  if(compute_atom_gradients) {
    CHECK(numposes == 1) << "Atomic gradient calculation not supported with numposes != 1";
    unsigned batch_size = top_shape[0];
    Dtype *diff = NULL;
    if(gpu) {
      diff = top[0]->mutable_gpu_diff();
      setAtomGradientsGPU(gmaker, diff, batch_size);
    }
    else {
      diff = top[0]->mutable_cpu_diff();
      for (int item_id = 0; item_id < batch_size; ++item_id) {

        int offset = item_id*example_size;
        Grids grids(diff+offset, boost::extents[numchannels][dim][dim][dim]);

        mol_transform& transform = batch_transform[item_id];
        gmaker.setCenter(transform.center[0], transform.center[1], transform.center[2]);
        gmaker.setAtomGradientsCPU(transform.mol.atoms, transform.mol.whichGrid, 
                transform.Q.boost(), grids, transform.mol.gradient);
      }
    }
  }
}

template <typename Dtype>
void MolGridDataLayer<Dtype>::Backward_relevance(const vector<Blob<Dtype>*>& top, const vector<bool>& propagate_down, const vector<Blob<Dtype>*>& bottom)
{

  CHECK(numposes == 1) << "Relevance calculations not supported with numposes != 1";

  Dtype *diff = top[0]->mutable_cpu_diff(); //TODO: implement gpu
  Dtype *data = top[0]->mutable_cpu_data();

  //propagate gradient grid onto atom positions
  unsigned batch_size = top_shape[0];
  for (int item_id = 0; item_id < batch_size; ++item_id) {

    int offset = item_id*example_size;
    Grids diffgrids(diff+offset, boost::extents[numchannels][dim][dim][dim]);
    Grids densegrids(data+offset, boost::extents[numchannels][dim][dim][dim]);
    mol_transform& transform = batch_transform[item_id];
    gmaker.setCenter(transform.center[0], transform.center[1], transform.center[2]);
    gmaker.setAtomRelevanceCPU(transform.mol.atoms, transform.mol.whichGrid, transform.Q.boost(),
        densegrids, diffgrids, transform.mol.gradient);
  }

  //float bottom_sum = 0.0;
  //for(int i = 0; i < bottom[0]->count(); i++)
  //{
  //        bottom_sum += bottom[0]->cpu_diff()[i];
  //}
  //std::cout << "MOLGRID BOTTOM: " << bottom_sum << '\n';


}



INSTANTIATE_CLASS(MolGridDataLayer);
REGISTER_LAYER_CLASS(MolGridData);

}  // namespace caffe
