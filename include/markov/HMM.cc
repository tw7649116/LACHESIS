///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// This software and its documentation are copyright (c) 2014-2015 by Joshua //
// N. Burton and the University of Washington.  All rights are reserved.     //
//                                                                           //
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS  //
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF                //
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT.  //
// IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY      //
// CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT //
// OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR  //
// THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////


// For general documentation, see HMM.h
#include "HMM.h"

// C and C++ includes
#include <assert.h>
#include <math.h> // exp, log
#include <stdio.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <numeric> // accumulate
#include <algorithm> // max_element







// Constructor.
// If N_symbols == 0, this is a continuous HMM; otherwise it's discrete.
HMM::HMM( const int N_states, const int N_symbols )
  : MarkovModel( N_states ),
    _N_symbols( N_symbols )
{
  // Clear the flags for loaded data.
  _has_symbol_emiss_probs = false;
  _has_observations       = false;
  _has_time_emiss_probs   = false;
  
  _ran_viterbi    = false;
  _ran_baum_welch = false;
  
}




HMM::~HMM() {}




// Use in discrete HMMs only.
// Load the probabilities of each state emitting each observable symbol.
// The input is an N_states x N_symbols matrix; probs[i][j] describes the
// probability of state i emitting symbol j.
void
HMM::SetSymbolEmissProbs( const vector< vector<double> > & probs )
{
  assert( is_discrete_HMM() );
  
  // Before using this probability set, verify that it makes sense.
  assert( int( probs.size() ) == _N_states );
  for ( int i = 0; i < _N_states; i++ )
    assert_prob_vector( probs[i], _N_symbols );
  
  // Convert the probabilities to log scale.
  _symbol_emiss_probs.resize( _N_states, vector<double>( _N_symbols ) );
  for ( int i = 0; i < _N_states; i++ )
    for ( int j = 0; j < _N_symbols; j++ )
      _symbol_emiss_probs[i][j] = log( probs[i][j] );
  
  _has_symbol_emiss_probs = true;
}




// Use in discrete HMMs only.
// Load the set of observed symbols.
void
HMM::SetObservations( const vector<int> & observations )
{
  assert( is_discrete_HMM() );
  
  _observations = observations;
  _has_observations = true;
}



// Use in continuous HMMs only.
// Load the log-likelihoods of each state emitting observed data at each
// timepoint.  Input these as logarithms.
// The input is an N_timepoints x N_states matrix (N_timepoints is inferred
// from the matrix); probs[i][j] describes the log-likelihood of the data at
// timepoint i being generated by state j.
// No matrix entries can be LOG_ZERO: that is, all states must be able to
// generate all observations.  If we allow LOG_ZERO, then we get the problem
// of WDAGs with no solution.
void
HMM::SetTimeEmissProbs( const vector< vector<double> > & probs )
{
  assert( !is_discrete_HMM() );
  
  // Find the number of timepoints.
  assert( !probs.empty() );
  size_t N_timepoints = probs.size();
  
  for ( size_t i = 0; i < N_timepoints; i++ )
    assert( (int)probs[i].size() == _N_states );
  
  _time_emiss_probs.resize( N_timepoints, vector<double>( _N_states ) );
  
  // The probabilities are already in log scale (which is necessary because
  // they can be tiny) so there's no need to convert.  However, we do 
  // semi-"normalize" them by adding constant factors, just to prevent possible
  // underflow.
  for ( size_t i = 0; i < N_timepoints; i++ ) {
    double max = *( max_element( probs[i].begin(), probs[i].end() ) );
    for ( int j = 0; j < _N_states; j++ ) {
      assert( probs[i][j] != LOG_ZERO );
      _time_emiss_probs[i][j] = probs[i][j] - max;
    }
  }
  
  _has_time_emiss_probs = true;
}




// Return true if this HMM has loaded all the data it needs to run training.
bool
HMM::HasAllData() const
{
  if ( !_has_init_probs  ) return false;
  if ( !_has_trans_probs ) return false;
  
  // Discrete HMMs need symbol emission probabilities and observations.
  if ( is_discrete_HMM() ) {
    if ( !_has_symbol_emiss_probs ) return false;
    if ( !_has_observations ) return false;
  }
  // Continuous HMMs need time emission probabilities.
  else
    if ( !_has_time_emiss_probs ) return false;
  
  return true;
}



// Create a WDAG representing the observed sequence, with weights according
// to the current parameters of this HMM.
// This WDAG will contain 2*N*M + 2 nodes, where N = number of states and M =
// number of observations.
// Note that the HMM probability parameters are stored as logarithms, so the
// edge weights are log probabilities.
// Edge names:
// Start:       S <init node>
// Transition:  T <source node> <target node>
// Emission:    E <node> <emitted symbol>
// Finish:      F
WDAG
HMM::to_WDAG() const
{
  assert( HasAllData() );
  
  WDAG wdag;
  wdag.Reserve( 2 * _N_states * NTimepoints() + 2 );
  
  // Create a pair of state vectors.  These will soon hold sets of WDAGNodes
  // representing the current state of the model.
  // stateA: The HMM has reached this state at this location in the sequence.
  // stateB: The HMM has observed the given symbol at this location.
  vector<WDAGNode *> stateA( _N_states, NULL );
  vector<WDAGNode *> stateB( _N_states, NULL );
  char edge_name[50];
  
  
  // Create the beginning node for the WDAG.
  WDAGNode * start_node = wdag.AddNode();
  wdag.SetReqStart( start_node );
  
  
  // Step through the set of observed symbols and extend the graph for each
  // observation.  Each observation adds 2N symbols to the graph, where N is
  // the number of states.
  for ( size_t t = 0; t < NTimepoints(); t++ ) {
    
    
    // Create a set of nodes representing the states at this location. 
    for ( int i = 0; i < _N_states; i++ ) {
      stateA[i] = wdag.AddNode();
      
      // If this is the first timepoint, create a special set of vertices.
      // The initial state probabilities are used here as edge weights.
      if ( t == 0 ) {
	sprintf( edge_name, "S %d", i ); // "S" = start
	stateA[i]->AddEdge( start_node, edge_name, _init_probs[i] );
      }
      
      // Otherwise, join each of these nodes to each of the second nodes from
      // the previous state - a total of N^2 states.  The edge weights are the
      // state transition probabilities.
      // These edges' names indicate which states they transition between.
      else
	for ( int i_prev = 0; i_prev < _N_states; i_prev++ ) {
	  sprintf( edge_name, "T %d %d", i_prev, i ); // "T" = transition
	  stateA[i]->AddEdge( stateB[i_prev], edge_name, _trans_probs[i_prev][i] );
	}
    }
    
    
    // Apply this symbol to the WDAG by adding a second node for each state.
    // The two nodes for the state are joined by an edge with a weight equal to
    // the state's emission probability for this observation.
    // These edges' names indicate which state is emitting which observation.
    for ( int i = 0; i < _N_states; i++ ) {
      
      // Find the probability of this state emitting this observation at this
      // timepoint.
      // If this is a discrete HMM, the emission probability is the probability
      // of this state producing the observed symbol of this timepoint.
      // If this is a continuous HMM, we look up the emission probability in
      // the matrix of time emission probabilities.
      int obs;
      double emiss_prob;
      if ( is_discrete_HMM() ) {
	obs = _observations[t];
	emiss_prob = _symbol_emiss_probs[i][obs];
      }
      else {
	obs = -1; // this is a dummy value used in edge naming only
	emiss_prob = _time_emiss_probs[t][i];
      }
      
      stateB[i] = wdag.AddNode();
      sprintf( edge_name, "E %d %d", i, obs ); // "E" = emission
      stateB[i]->AddEdge( stateA[i], edge_name, emiss_prob );
    }
    
  }
  
  
  
  // Finally, create the ending node.  This node's input weights are all 0.
  WDAGNode * end_node = wdag.AddNode();
  for ( int i = 0; i < _N_states; i++ )
    end_node->AddEdge( stateB[i], "F", 0 ); // "F" = finish
  wdag.SetReqEnd( end_node );
  
  
  assert( (size_t) wdag.N() == 2 * _N_states * NTimepoints() + 2 );
  
  return wdag;
  
}


static istringstream * iss;



// As part of Viterbi training, apply a path to get new probabilities.
// Also determine the sequence of states in the best path.
// Return true if any of the probabilities change.
bool
HMM::AdjustProbsToViterbi( const vector<string> & best_path, vector<int> & states )
{
  assert( !best_path.empty() ); // if this fails, the WDAG has failed - typically because there is no way to get from the beginning to the end of the path due to transmission/emission probabilities that are 0
  
  // Get the names of the edges in the best path, and tally the number of times
  // each edge name appears.  Thus find the observed transition and
  // emission probabilities.
  vector< vector<int> > trans_counts( _N_states, vector<int>( _N_states, 0 ) );
  vector< vector<int> > emiss_counts( _N_states, vector<int>( _N_symbols,0 ) );
  vector<int> state_counts( _N_states, 0 );
  states.clear();
  
  for ( size_t i = 0; i < best_path.size(); i++ ) {
    
    // The edges we care about have names of the format "E x y" or "T x z",
    // where x,y are state IDs and z is an observed symbol.
    iss = new istringstream( best_path[i] );
    char type;
    int S1, S2;
    *iss >> type >> S1 >> S2;
    assert( type == 'S' || type == 'T' || type == 'E' || type == 'F' );
    
    // Transition edges.
    if ( type == 'T' ) trans_counts[S1][S2]++;
    
    // Emission edges.
    if ( type == 'E' ) {
      if ( is_discrete_HMM() ) emiss_counts[S1][S2]++;
      state_counts[S1]++;
      states.push_back(S1);
    }
    
    delete iss;
  }
  
  assert( states.size() == NTimepoints() );
  
  bool change = false;
  
  // Determine the frequency with which each state appears in the best path.
  _state_freqs.resize( _N_states );
  for ( int i = 0; i < _N_states; i++ )
    _state_freqs[i] = double( state_counts[i] ) / NTimepoints();
  
  
  // Normalize the observed transition probabilities, then set them as the
  // theoretical probabilities, thus completing the Viterbi training.
  for ( int i = 0; i < _N_states; i++ ) {
    int total = accumulate( trans_counts[i].begin(), trans_counts[i].end(), 0 );
    
    for ( int j = 0; j < _N_states; j++ ) {
      
      // If a state never occurs in the Viterbi iteration, add pseudocounts
      // to make it equally likely to transition to any other state.
      double new_prob =
	total == 0 ?
	-log( _N_states ) :
	log( double( trans_counts[i][j] ) / total );
      
      if ( _trans_probs[i][j] != new_prob ) change = true;
      _trans_probs[i][j] = new_prob;
    }
  }
  
  // If this is a discrete HMM, reset the symbol emission probabilities in the
  // same manner as the transition probabilities.
  if ( is_discrete_HMM() )
    for ( int i = 0; i < _N_states; i++ ) {
      int total = accumulate( emiss_counts[i].begin(), emiss_counts[i].end(), 0 );
      
      for ( int j = 0; j < _N_symbols; j++ ) {
	
	// If a state never occurs in the Viterbi iteration, add pseudocounts
	// to make it equally likely to emit any symbol.
	double new_prob =
	  total == 0 ?
	  -log( _N_symbols ) :
	  log( double( emiss_counts[i][j] ) / total );
	
	if ( _symbol_emiss_probs[i][j] != new_prob ) change = true;
	_symbol_emiss_probs[i][j] = new_prob;
      }
    }
  
  
  
  // Return true if any of the probabilities have changed.
  return change;
}







// As part of Baum-Welch training, find the posterior probabilities of each
// transition and each emission, and apply these to get new probabilities.
// Return true if any of the probabilities change.
bool
HMM::AdjustProbsToBaumWelch( const WDAG & wdag )
{
  
  // These variables will contain sums of the total weights of all transition,
  // emission, and initiation edges.
  vector<double> new_init_probs( _N_states, LOG_ZERO );
  vector< vector<double> > new_trans_probs( _N_states, vector<double>( _N_states,  LOG_ZERO ) );
  vector< vector<double> > new_emiss_probs( _N_states, vector<double>( _N_symbols, LOG_ZERO ) );
  vector<double> new_state_freqs( _N_states, LOG_ZERO );
  
  size_t n_emissions = 0;
  
  
  // Loop over all edges in the WDAG.  We do this indirectly, by looping over
  // all nodes and then by those nodes' in-edges.
  for ( int i = 0; i < wdag.N(); i++ ) {
    const WDAGNode * child = wdag.GetNode(i);
    
    for ( int j = 0; j < child->_n_parents; j++ ) {
      const WDAGNode * parent = child->_parents[j];
      string edge_name = child->_in_e_names[j];
      double edge_weight = child->_in_e_weights[j];
      
      // Calculate the posterior probability of this edge.
      double p_prob = parent->_fw_prob + child->_bw_prob + edge_weight;
      
      // Parse this edge's name to determine what kind of edge it is.
      // Initiation edges are of the format "S x".
      // Transition edge names are of the format "T x y", while emission edge
      // names are of the format "E x z".  x,y = state IDs; z = symbol char.
      iss = new istringstream( edge_name );
      char type;
      int S1, S2;
      *iss >> type >> S1 >> S2;
      assert( type == 'S' || type == 'T' || type == 'E' || type == 'F' );
      
      // Initiation edge.
      if ( type == 'S' )
	new_init_probs[S1] = p_prob;
      
      // Transition edge.
      else if ( type == 'T' ) {
	new_trans_probs[S1][S2] = lnsum( new_trans_probs[S1][S2], p_prob );
	if ( isnan( new_trans_probs[S1][S2] ) )
	     cout << "EDGE: " << edge_name << "\tPROBS: " << parent->_fw_prob << " + " << child->_bw_prob << " + " << edge_weight << " = " << p_prob << "\tTRANS PROB GOES TO " << new_trans_probs[S1][S2] << endl;
      }
      
      // Emission edge.
      else if ( type == 'E' ) {
	if ( is_discrete_HMM() )
	  new_emiss_probs[S1][S2] = lnsum( new_emiss_probs[S1][S2], p_prob );
	new_state_freqs[S1] = lnsum( new_state_freqs[S1], p_prob );
	n_emissions++;
      }
      
      
      delete iss;
    }
  }
  
      
  assert( n_emissions == NTimepoints() * _N_states );
  
  bool change = false;
  
  
  // Normalize to determine the frequency with which each state appears in the
  // average path.
  double denom = LOG_ZERO;
  for ( int j = 0; j < _N_states; j++ )
    denom = lnsum( denom, new_state_freqs[j] );
  _state_freqs.resize( _N_states );
  for ( int j = 0; j < _N_states; j++ )
    _state_freqs[j] = exp( new_state_freqs[j] - denom );
  
  
  // Normalize the observed initiation probabilities, then set them as the
  // theoretical probabilities.
  denom = LOG_ZERO;
  for ( int j = 0; j < _N_states; j++ )
    denom = lnsum( denom, new_init_probs[j] );
  
  for ( int j = 0; j < _N_states; j++ ) {
    if ( _init_probs[j] != new_init_probs[j] - denom ) change = true;
    _init_probs[j] = new_init_probs[j] - denom;
  }
  
  
  // Normalize the observed transition probabilities, then set them as the
  // theoretical probabilities, thus completing the Viterbi training.
  for ( int i = 0; i < _N_states; i++ ) {
    denom = LOG_ZERO;
    for ( int j = 0; j < _N_states; j++ )
      denom = lnsum( denom, new_trans_probs[i][j] );
    
    for ( int j = 0; j < _N_states; j++ ) {
      if ( _trans_probs[i][j] != new_trans_probs[i][j] - denom ) change = true;
      _trans_probs[i][j] = new_trans_probs[i][j] - denom;
    }
  }
  
  
  // If this is a discrete HMM, reset the symbol emission probabilities in the
  // same manner as the transition probabilities.
  if ( is_discrete_HMM() )
    for ( int i = 0; i < _N_states; i++ ) {
      denom = LOG_ZERO;
      for ( int j = 0; j < _N_symbols; j++ )
	denom = lnsum( denom, new_emiss_probs[i][j] );
      
      for ( int j = 0; j < _N_symbols; j++ ) {
	if ( _symbol_emiss_probs[i][j] != new_emiss_probs[i][j] - denom ) change = true;
	_symbol_emiss_probs[i][j] = new_emiss_probs[i][j] - denom;
      }
    }
  
  
  
  // Return true if any of the probabilities have changed.
  return change;
}




// Viterbi training to improve the transition probabilities.
// Return true if any of the probabilities change.
// Also outputs the hidden states corresponding to each symbol: specifically,
// returns a vector of the same length as the observations vector, but with
// state IDs instead of symbol IDs.
// For iterative training, run this function repeatedly.
bool
HMM::ViterbiTraining( vector<int> & predicted_states )
{
  assert( HasAllData() );
  
  // Create a WDAG for this HMM.
  WDAG wdag = to_WDAG();
  
  // Compute the highest-weight path on this WDAG.
  wdag.FindBestPath();
  
  
  // Apply the highest-weight path to find the probabilities and predict the
  // set of hidden states.
  bool change = AdjustProbsToViterbi( wdag._best_edges, predicted_states );
  
  _ran_viterbi = true;
  
  return change;
}




// Baum-Welch training to improve the transition probabilities.
// Return true if any of the probabilities change.
// Also output the log likelihood of the entire HMM.
// For iterative training, run this function repeatedly.
bool
HMM::BaumWelchTraining( double & log_like )
{
  assert( HasAllData() );
  
  // Create a WDAG for this HMM.
  WDAG wdag = to_WDAG();
  
  // Run the highest-weight-path on this WDAG and find all of the
  // forward and backward probabilities.
  wdag.FindPosteriorProbs();
  
  
  // Combine the forward and backward probabilities to find the posterior
  // probability of each transition and each emission.
  bool change = AdjustProbsToBaumWelch( wdag );
  
  _ran_baum_welch = true;
  
  log_like = wdag.Alpha() / log(2);
  return change;
}








// Return the number of timepoints.
// This depends slightly on whether this is a discrete or continuous HMM.
size_t
HMM::NTimepoints() const
{
  assert( HasAllData() );
  if ( is_discrete_HMM() ) return _observations.size();
  else return _time_emiss_probs.size();
}




// Draw a PNG image representing the graph in the vicinity of a timepoint.
// T = the timepoint centered on - must be less than NTimepoints()!
// depth = the number of timepoints in each direction to go
// Note that edges with weights of -infinity are not drawn.
// This uses GraphViz/DOT to draw a *.dot file, and then converts it to a
// PNG file.
// TODO: this still doesn't work very well in edge cases
void
HMM::DrawPNGAtState( const string & PNG_file_head, const size_t T, const size_t depth ) const
{
  assert( T < NTimepoints() );
  
  
  // Figure out the range of timepoints that we'll be dealing with.
  size_t min_t = T > depth ? T - depth : 0;
  size_t max_t = min( T + depth, NTimepoints() - 1 );
  
  
  
  // Open up a digraph that will illustrate the edges between the nodes.
  // Write the digraph as a DOT format file.
  ofstream DOT( ( PNG_file_head + ".dot" ).c_str(), ios::out );
  DOT << "digraph HMM_at_state_" << T << " {\n";
  
  // Each timepoint T is represented in the WDAG as two seta (N1, N2) of nodes.
  // Each set has _N_states nodes.
  // Transitions from the N2 state of timepoint T-1 to the N1 state of
  // timepoint T represent state transitions between T-1 and T.
  // Transitions from the N1 state of timepoint T to the N2 state of
  // timepoint T represent emissions at T.
  // For each timepoint, we must make two sets of edges in our graph: one
  // representing state transitions from N2(T-1) to N1(T), and one representing
  // emissions from N1(T) to N2(T).
  for ( size_t t = min_t; t <= max_t; t++ ) {
  
    // Find the ID of the two sets of nodes N1(t), N2(t) that represent
    // timepoint t, as well as the set N2(t-1).
    // Each set has _N_states nodes, except at edge cases.
    vector<int> N2_tm1, N1, N2;
    for ( int i = 0; i < _N_states; i++ ) {
      N2_tm1.push_back( _N_states * (2*t+1) + 1 + i );
      N1    .push_back( _N_states * (2*t+2) + 1 + i );
      N2    .push_back( _N_states * (2*t+3) + 1 + i );
    }
    
    // Assign labels to the vertices.
    for ( int i = 0; i < _N_states; i++ ) {
      if ( t == 0 ) DOT << "0 [label=\"START\"]\n";
      else          DOT << N2_tm1[i] << " [label=\"" << t << "_" << i << "_T\"]\n";
      DOT << N1[i] << " [label=\"" << t+1 << "_" << i << "_E\"]\n";
      DOT << N2[i] << " [label=\"" << t+1 << "_" << i << "_T\"]\n";
    }
    
    // Create digraph adjacencies representing the transitions from t-1 to t.
    for ( int i = 0; i < _N_states; i++ )
      for ( int j = 0; j < _N_states; j++ )
	if ( isfinite( _trans_probs[i][j] ) )
	  DOT << N2_tm1[i] << " -> " << N1[j] << " [ label = \"T_" << exp(_trans_probs[i][j]) << "\" ];\n";
    
    // Create digraph adjacencies representing the emissions at time t.
    for ( int i = 0; i < _N_states; i++ ) {
      double emiss_prob;
      if ( is_discrete_HMM() ) emiss_prob = _symbol_emiss_probs[i][ _observations[t] ];
      else emiss_prob = _time_emiss_probs[t][i];
      if ( isfinite( emiss_prob ) )
	DOT << N1[i] << " -> " << N2[i] << " [ label = \"E_" << exp(emiss_prob) << "\" ];\n";
    }
    
  }
  
  
  
  DOT << "}\n";
  
  DOT.close();
  
  
  // Now automatically convert the DOT file into a PNG file.
  string cmd = "dot -Tpng " + PNG_file_head + ".dot > ~/public_html/" + PNG_file_head + ".png";
  system( cmd.c_str() );
  cmd = ( "rm " + PNG_file_head + ".dot" );
  system( cmd.c_str() );
}



// Print out useful information about the current state of this HMM.
// NOTE: When printing probabilities, convert from logarithmic scale back to
// regular scale.
void
HMM::Print( ostream & out ) const
{
  static const int MAX_N_SYMBOLS = 200;
  
  char num[50];
  
  out << "HIDDEN MARKOV MODEL" << endl;
  out << _N_states << " states" << endl;
  if ( is_discrete_HMM() )
    out << "Discrete HMM with " << _N_symbols << " observable symbols over " << NTimepoints() << " timepoints" << endl;
  else
    out << "Continuous HMM with " << NTimepoints() << " timepoints" << endl;
  out << endl;
  
  
  // Print initial state probabilities.
  out << "Initial state probabilities:";
  if ( _has_init_probs ) {
    out << "\t\t";
    for ( int i = 0; i < _N_states; i++ ) {
      sprintf( num, "%.5f", exp(_init_probs[i]) );
      out << "\t" << num;
    }
    out << endl;
  }
  else
    out << "\t\t\tNOT LOADED" << endl;
  
  
  // Print state-to-state transition probabilities.
  out << "State-to-state transition probabilities:";
  if ( _has_trans_probs ) {
    
    // Print header line for matrix chart.
    out << endl;
    for ( int j = 0; j < _N_states; j++ )
      out << "\tS" << j+1;
    out << endl;
    
    // Print matrix chart.
    for ( int i = 0; i < _N_states; i++ ) {
      out << "S" << i+1;
      for ( int j = 0; j < _N_states; j++ ) {
	sprintf( num, "%.5f", exp(_trans_probs[i][j]) );
	out << "\t" << num;
      }
      out << endl;
    }
    out << endl;
    
  }
  else
    out << "\tNOT LOADED" << endl;
  
  
  
  // Print the probability of each state emitting each symbol.
  // If there are a lot of observations, don't print them all.
  if ( is_discrete_HMM() ) {
    out << "Symbol emission probabilities:";
    if ( _has_symbol_emiss_probs && _N_symbols <= MAX_N_SYMBOLS ) {
      
      // Print header line for matrix chart.
      out << endl;
      for ( int j = 0; j < _N_symbols; j++ )
	out << "\tSYM" << j;
      out << endl;
      
      // Print matrix chart.
      for ( int i = 0; i < _N_states; i++ ) {
	out << "S" << i+1;
	for ( int j = 0; j < _N_symbols; j++ ) {
	  sprintf( num, "%.5f", exp(_symbol_emiss_probs[i][j]) );
	  out << "\t" << num;
	}
	out << endl;
      }
      out << endl;
      
    }
    else if ( _has_symbol_emiss_probs )
      out << "\t\t\t<matrix of size " << _N_states << " states X " << _N_symbols << " symbols>" << endl;
    else
      out << "\t\t\tNOT LOADED" << endl;
    
    
    out << "Sequence of observed symbols:";
    if ( _has_observations ) {
      out << "\t\t\t<sequence of length " << NTimepoints() << ">" << endl;
    }
    else
      out << "\t\t\tNOT LOADED" << endl;
  }
  
  else { // continuous HMMs
    out << "Time emission probabilities:" << endl;
    out << "\t\t\t<matrix of size " << _N_states << " states X " << NTimepoints() << " timepoints>" << endl;
  }
    
  out << endl;
}




