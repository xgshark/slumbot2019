// head_to_head assesses head to head results between two systems with two nice features:
// 1) You can sample final street boards evaluated leading to drastic speedups at the expense
// of some accuracy.
// 2) You can tell either or both systems to resolve subgames.
//
// Unlike play we do not sample hands.  In the simplest scenario, we traverse the betting tree,
// tracking each player's range at each node.  At terminal nodes we evaluate range vs. range EV.
//
// This works nicely with resolving.  We only need to resolve a given subgame at most once.  If we
// sampling, we only resolve the subgames needed.
//
// I don't think I can support multiplayer.
//
// We don't support asymmetric abstractions.
//
// Currently only support resolving on max street.  Resolving prior to max street partially
// supported.  Can we still sample final street boards?  I guess, but we will resolve the
// entire turn subtree.  I need a hand tree for the resolve which is different from the
// regular hand tree.
//
// Is there much wasted work recomputing reach probs for streets prior to the final street?
// A bit, but maybe it's not significant.  May only be sampling some turn boards so it might
// be a waste to precompute turn reach probs for all turn boards.  Could do the preflop and maybe
// the flop.

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "betting_abstraction.h"
#include "betting_abstraction_params.h"
#include "betting_tree.h"
#include "board_tree.h"
#include "buckets.h"
#include "canonical_cards.h"
#include "canonical.h"
#include "card_abstraction.h"
#include "card_abstraction_params.h"
#include "cfr_config.h"
#include "cfr_params.h"
#include "cfr_values.h"
#include "constants.h"
#include "files.h"
#include "game.h"
#include "game_params.h"
#include "hand_tree.h"
#include "hand_value_tree.h"
#include "io.h"
#include "params.h"
#include "rand.h"
#include "sorting.h"
#include "subgame_utils.h"
#include "unsafe_eg_cfr.h"

using std::pair;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;

class Player {
public:
  Player(const BettingAbstraction &a_ba, const BettingAbstraction &b_ba,
	 const CardAbstraction &a_ca, const CardAbstraction &b_ca, const CFRConfig &a_cc,
	 const CFRConfig &b_cc, int a_it, int b_it, int resolve_st, bool resolve_a, bool resolve_b,
	 const CardAbstraction &as_ca, const BettingAbstraction &as_ba, const CFRConfig &as_cc,
	 const CardAbstraction &bs_ca, const BettingAbstraction &bs_ba, const CFRConfig &bc_cc);
  ~Player(void) {}
  void Go(int num_sampled_max_street_boards);
private:
  void Showdown(Node *a_node, Node *b_node, shared_ptr<double []> *reach_probs);
  void Fold(Node *a_node, Node *b_node, shared_ptr<double []> *reach_probs);
  shared_ptr<double []> **GetSuccReachProbs(Node *node, int gbd, const Buckets &buckets,
					    const CFRValues *sumprobs,
					    shared_ptr<double []> *reach_probs);
  void Nonterminal(Node *a_node, Node *b_node, const string &action_sequence,
		   shared_ptr<double []> *reach_probs);
  void Walk(Node *a_node, Node *b_node, const string &action_sequence,
	    shared_ptr<double []> *reach_probs, int last_st);
  void ProcessMaxStreetBoard(int msbd);

  const BettingAbstraction &a_subgame_betting_abstraction_;
  const BettingAbstraction &b_subgame_betting_abstraction_;
  unique_ptr<BettingTree> a_betting_tree_;
  unique_ptr<BettingTree> b_betting_tree_;
  shared_ptr<Buckets> a_base_buckets_;
  shared_ptr<Buckets> b_base_buckets_;
  shared_ptr<CFRValues> a_probs_;
  shared_ptr<CFRValues> b_probs_;
  int resolve_st_;
  bool resolve_a_;
  bool resolve_b_;
  // When we resolve a street, the board index may change.  This is why we have separate
  // a_boards_ and b_boards_.  Only one player may be resolving.
  unique_ptr<int []> a_boards_;
  unique_ptr<int []> b_boards_;
  // The number of times we sampled this board.
  int num_samples_;
  int msbd_;
  int b_pos_;
  shared_ptr<HandTree> hand_tree_;
  shared_ptr<HandTree> resolve_hand_tree_;
  const CanonicalCards *hands_;
  double sum_b_outcomes_;
  double sum_p0_outcomes_;
  double sum_p1_outcomes_;
  double sum_weights_;
  shared_ptr<Buckets> a_subgame_buckets_;
  shared_ptr<Buckets> b_subgame_buckets_;
  unique_ptr<BettingTree> a_subtree_;
  unique_ptr<BettingTree> b_subtree_;
  unique_ptr<EGCFR> a_eg_cfr_;
  unique_ptr<EGCFR> b_eg_cfr_;
  int num_subgame_its_;
  unique_ptr< unique_ptr<int []> []> ms_hcp_to_raw_hcp_;
  int num_resolves_;
  double resolving_secs_;
};

Player::Player(const BettingAbstraction &a_ba, const BettingAbstraction &b_ba,
	       const CardAbstraction &a_ca, const CardAbstraction &b_ca, const CFRConfig &a_cc,
	       const CFRConfig &b_cc, int a_it, int b_it, int resolve_st, bool resolve_a,
	       bool resolve_b, const CardAbstraction &as_ca, const BettingAbstraction &as_ba,
	       const CFRConfig &as_cc, const CardAbstraction &bs_ca,
	       const BettingAbstraction &bs_ba, const CFRConfig &bs_cc):
  a_subgame_betting_abstraction_(as_ba), b_subgame_betting_abstraction_(bs_ba) {
  int max_street = Game::MaxStreet();
  a_boards_.reset(new int[max_street + 1]);
  b_boards_.reset(new int[max_street + 1]);
  a_boards_[0] = 0;
  b_boards_[0] = 0;
  sum_b_outcomes_ = 0;
  sum_p0_outcomes_ = 0;
  sum_p1_outcomes_ = 0;
  sum_weights_ = 0;
  resolve_a_ = resolve_a;
  resolve_b_ = resolve_b;
  resolve_st_ = resolve_st;
  num_subgame_its_ = 200;

  a_base_buckets_.reset(new Buckets(a_ca, false));
  if (a_ca.CardAbstractionName() == b_ca.CardAbstractionName()) {
    fprintf(stderr, "Sharing buckets\n");
    b_base_buckets_ = a_base_buckets_;
  } else {
    fprintf(stderr, "Not sharing buckets\n");
    b_base_buckets_.reset(new Buckets(b_ca, false));
  }
  BoardTree::Create();
  BoardTree::CreateLookup();
  BoardTree::BuildBoardCounts();
  BoardTree::BuildPredBoards();

  a_betting_tree_.reset(new BettingTree(a_ba));
  b_betting_tree_.reset(new BettingTree(b_ba));

  bool shared_probs = 
    (a_ca.CardAbstractionName().c_str() == b_ca.CardAbstractionName() &&
     a_ba.BettingAbstractionName().c_str() == b_ba.BettingAbstractionName() &&
     a_cc.CFRConfigName() == b_cc.CFRConfigName() && a_it == b_it);
  unique_ptr<bool []> a_streets(new bool[max_street + 1]);
  unique_ptr<bool []> b_streets(new bool[max_street + 1]);
  if (resolve_a_ && resolve_b_) {
    for (int st = 0; st <= max_street; ++st) {
      a_streets[st] = (st < resolve_st_);
      b_streets[st] = (st < resolve_st_);
    }
  } else if (! resolve_a_ && ! resolve_b_) {
    for (int st = 0; st <= max_street; ++st) {
      a_streets[st] = true;
      b_streets[st] = true;
    }
  } else {
    // One system is being resolved
    if (shared_probs) {
      for (int st = 0; st <= max_street; ++st) {
	a_streets[st] = true;
	b_streets[st] = true;
      }
    } else {
      for (int st = 0; st <= max_street; ++st) {
	a_streets[st] = (st < resolve_st_ || ! resolve_a_);
	b_streets[st] = (st < resolve_st_ || ! resolve_b_);
      }
    }
  }
  
  a_probs_.reset(new CFRValues(nullptr, a_streets.get(), 0, 0, *a_base_buckets_,
			       a_betting_tree_.get()));
  char dir[500];
  
  sprintf(dir, "%s/%s.%u.%s.%u.%u.%u.%s.%s", Files::OldCFRBase(),
	  Game::GameName().c_str(), Game::NumPlayers(),
	  a_ca.CardAbstractionName().c_str(),
	  Game::NumRanks(), Game::NumSuits(), Game::MaxStreet(),
	  a_ba.BettingAbstractionName().c_str(),
	  a_cc.CFRConfigName().c_str());
  // Note assumption that we can use the betting tree for position 0
  a_probs_->Read(dir, a_it, a_betting_tree_->Root(), "x", -1, true);

  if (a_ca.CardAbstractionName().c_str() == b_ca.CardAbstractionName() &&
      a_ba.BettingAbstractionName().c_str() == b_ba.BettingAbstractionName() &&
      a_cc.CFRConfigName() == b_cc.CFRConfigName() && a_it == b_it ) {
    fprintf(stderr, "Sharing probs between A and B\n");
    b_probs_ = a_probs_;
  } else {
    fprintf(stderr, "A and B do not share probs\n");
    b_probs_.reset(new CFRValues(nullptr, b_streets.get(), 0, 0, *b_base_buckets_,
				 b_betting_tree_.get()));
    sprintf(dir, "%s/%s.%u.%s.%u.%u.%u.%s.%s", Files::OldCFRBase(), Game::GameName().c_str(),
	    Game::NumPlayers(), b_ca.CardAbstractionName().c_str(), Game::NumRanks(),
	    Game::NumSuits(), Game::MaxStreet(), b_ba.BettingAbstractionName().c_str(),
	    b_cc.CFRConfigName().c_str());
    b_probs_->Read(dir, b_it, b_betting_tree_->Root(), "x", -1, true);
  }

  // Check for dups for buckets
  if (resolve_a_) {
    a_subgame_buckets_.reset(new Buckets(as_ca, false));
    a_eg_cfr_.reset(new UnsafeEGCFR(as_ca, a_ca, as_ba, a_ba, as_cc, a_cc, *a_subgame_buckets_, 1));
  }
  if (resolve_b_) {
    b_subgame_buckets_.reset(new Buckets(bs_ca, false));
    b_eg_cfr_.reset(new UnsafeEGCFR(bs_ca, b_ca, bs_ba, b_ba, bs_cc, b_cc, *b_subgame_buckets_, 1));
  }
  // We have two different ways of indexing hole card pairs.  On the final street we sort the
  // hole card pairs by strength and index them accordingly.  There is also the "raw" hole card
  // pair indexing which is ordered by rank (i.e., 2d2c is index 0 and so forth).  On all streets
  // prior to the final street, the only index of interest is the raw index.  When looking up
  // a bucket, we also only care about the raw index.
  // Here we initialize a map which will let us go from final street strength-sorted hole card
  // pair indices to the raw index on any street.
  int num_ms_hole_card_pairs = Game::NumHoleCardPairs(max_street);
  ms_hcp_to_raw_hcp_.reset(new unique_ptr<int []>[max_street + 1]);
  for (int st = 0; st <= max_street; ++st) {
    ms_hcp_to_raw_hcp_[st].reset(new int[num_ms_hole_card_pairs]);
  }
}

// Compute outcome from B's perspective
void Player::Showdown(Node *a_node, Node *b_node, shared_ptr<double []> *reach_probs) {
  Card max_card1 = Game::MaxCard() + 1;

  double *a_probs = b_pos_ == 0 ? reach_probs[1].get() : reach_probs[0].get();
  double *b_probs = b_pos_ == 0 ? reach_probs[0].get() : reach_probs[1].get();
  unique_ptr<double []> cum_opp_card_probs(new double[52]);
  unique_ptr<double []> total_opp_card_probs(new double[52]);
  for (Card c = 0; c < max_card1; ++c) {
    cum_opp_card_probs[c] = 0;
    total_opp_card_probs[c] = 0;
  }
  int max_street = Game::MaxStreet();
  int num_hole_card_pairs = Game::NumHoleCardPairs(max_street);
  double sum_opp_probs = 0;
  for (int hcp = 0; hcp < num_hole_card_pairs; ++hcp) {
    const Card *cards = hands_->Cards(hcp);
    Card hi = cards[0];
    Card lo = cards[1];
    int enc = hi * max_card1 + lo;
    double opp_prob = a_probs[enc];
    total_opp_card_probs[hi] += opp_prob;
    total_opp_card_probs[lo] += opp_prob;
    sum_opp_probs += opp_prob;
    if (opp_prob > 1.0) {
      fprintf(stderr, "Showdown: opp_prob (a) %f hcp %u\n", opp_prob, hcp);
      exit(-1);
    }
  }

  double opp_cum_prob = 0;
  unique_ptr<double []> win_probs(new double[num_hole_card_pairs]);
  double half_pot = a_node->LastBetTo();
  double sum_our_vals = 0, sum_joint_probs = 0;

  int j = 0;
  while (j < num_hole_card_pairs) {
    int last_hand_val = hands_->HandValue(j);
    int begin_range = j;
    // Make three passes through the range of equally strong hands
    // First pass computes win counts for each hand and finds end of range
    // Second pass updates cumulative counters
    // Third pass computes lose counts for each hand
    while (j < num_hole_card_pairs) {
      const Card *cards = hands_->Cards(j);
      Card hi = cards[0];
      Card lo = cards[1];
      int hand_val = hands_->HandValue(j);
      if (hand_val != last_hand_val) break;
      win_probs[j] = opp_cum_prob - cum_opp_card_probs[hi] - cum_opp_card_probs[lo];
      ++j;
    }
    // Positions begin_range...j-1 (inclusive) all have the same hand value
    for (int k = begin_range; k < j; ++k) {
      const Card *cards = hands_->Cards(k);
      Card hi = cards[0];
      Card lo = cards[1];
      int enc = hi * max_card1 + lo;
      double opp_prob = a_probs[enc];
      if (opp_prob <= 0) continue;
      cum_opp_card_probs[hi] += opp_prob;
      cum_opp_card_probs[lo] += opp_prob;
      opp_cum_prob += opp_prob;
    }
    for (int k = begin_range; k < j; ++k) {
      const Card *cards = hands_->Cards(k);
      Card hi = cards[0];
      Card lo = cards[1];
      int enc = hi * max_card1 + lo;
      double our_prob = b_probs[enc];
      double better_hi_prob = total_opp_card_probs[hi] - cum_opp_card_probs[hi];
      double better_lo_prob = total_opp_card_probs[lo] - cum_opp_card_probs[lo];
      double lose_prob = (sum_opp_probs - opp_cum_prob) - better_hi_prob - better_lo_prob;
      sum_our_vals += our_prob * (win_probs[k] - lose_prob) * half_pot;
      // This is the sum of all A reach probabilities consistent with B holding <hi, lo>.
      double sum_consistent_opp_probs = sum_opp_probs + a_probs[enc] -
	total_opp_card_probs[hi] - total_opp_card_probs[lo];
      sum_joint_probs += our_prob * sum_consistent_opp_probs;
    }
  }

  // Scale to account for frequency of board
  double wtd_sum_our_vals = sum_our_vals * (double)num_samples_;
  double wtd_sum_joint_probs = sum_joint_probs * (double)num_samples_;
  sum_b_outcomes_ += wtd_sum_our_vals;
  if (b_pos_ == 0) {
    sum_p0_outcomes_ += wtd_sum_our_vals;
    sum_p1_outcomes_ -= wtd_sum_our_vals;
  } else {
    sum_p0_outcomes_ -= wtd_sum_our_vals;
    sum_p1_outcomes_ += wtd_sum_our_vals;
  }
  sum_weights_ += wtd_sum_joint_probs;
}
  
// Compute outcome from B's perspective
void Player::Fold(Node *a_node, Node *b_node, shared_ptr<double []> *reach_probs) {
  Card max_card1 = Game::MaxCard() + 1;

  double half_pot = a_node->LastBetTo();
  // Player acting encodes player remaining at fold nodes
  int rem_p = a_node->PlayerActing();
  // Outcomes are from B's perspective
  if (b_pos_ != rem_p) {
    // B has folded
    half_pot = -half_pot;
  }
  double sum_our_vals = 0, sum_joint_probs = 0;

  double *a_probs = b_pos_ == 0 ? reach_probs[1].get() : reach_probs[0].get();
  double *b_probs = b_pos_ == 0 ? reach_probs[0].get() : reach_probs[1].get();
  unique_ptr<double []> cum_opp_card_probs(new double[52]);
  unique_ptr<double []> total_opp_card_probs(new double[52]);
  for (Card c = 0; c < max_card1; ++c) {
    cum_opp_card_probs[c] = 0;
    total_opp_card_probs[c] = 0;
  }

  double sum_opp_probs = 0;
  int max_street = Game::MaxStreet();
  // Always iterate through hole card pairs consistent with the sampled *max street* board, even if
  // this is a pre-max-street node.
  int num_hole_card_pairs = Game::NumHoleCardPairs(max_street);
  for (int hcp = 0; hcp < num_hole_card_pairs; ++hcp) {
    const Card *cards = hands_->Cards(hcp);
    Card hi = cards[0];
    Card lo = cards[1];
    int enc = hi * max_card1 + lo;
    double opp_prob = a_probs[enc];
    total_opp_card_probs[hi] += opp_prob;
    total_opp_card_probs[lo] += opp_prob;
    sum_opp_probs += opp_prob;
  }

  for (int i = 0; i < num_hole_card_pairs; ++i) {
    const Card *cards = hands_->Cards(i);
    Card hi = cards[0];
    Card lo = cards[1];
    int enc = hi * max_card1 + lo;
    double our_prob = b_probs[enc];
    // This is the sum of all the A reach probabilities consistent with B holding <hi, lo>.
    double sum_consistent_opp_probs = sum_opp_probs + a_probs[enc] -
      total_opp_card_probs[hi] - total_opp_card_probs[lo];
    sum_our_vals += our_prob * half_pot * sum_consistent_opp_probs;
    sum_joint_probs += our_prob * sum_consistent_opp_probs;    
  }

  // Scale to account for frequency of board
  double wtd_sum_our_vals = sum_our_vals * num_samples_;
  double wtd_sum_joint_probs = sum_joint_probs * num_samples_;
  sum_b_outcomes_ += wtd_sum_our_vals;
  if (b_pos_ == 0) {
    sum_p0_outcomes_ += wtd_sum_our_vals;
    sum_p1_outcomes_ -= wtd_sum_our_vals;
  } else {
    sum_p0_outcomes_ -= wtd_sum_our_vals;
    sum_p1_outcomes_ += wtd_sum_our_vals;
  }
  sum_weights_ += wtd_sum_joint_probs;
}
  
// Hard-coded for heads-up
shared_ptr<double []> **Player::GetSuccReachProbs(Node *node, int gbd, const Buckets &buckets,
						  const CFRValues *sumprobs,
						  shared_ptr<double []> *reach_probs) {
  int num_succs = node->NumSuccs();
  shared_ptr<double []> **succ_reach_probs = new shared_ptr<double []> *[num_succs];
  int max_card1 = Game::MaxCard() + 1;
  int num_enc = max_card1 * max_card1;
  int st = node->Street();
  int max_street = Game::MaxStreet();
  // For some purposes below, we care about the number of hole card pairs on the final street.
  // (We are maintaining probabilities for every max street hand for the sampled max street
  // board.)  For other purposes, we care about the number of hole card pairs on the current
  // street (for looking up the probability of the current actions).
  int num_ms_hole_card_pairs = Game::NumHoleCardPairs(max_street);
  int num_st_hole_card_pairs = Game::NumHoleCardPairs(st);
  for (int s = 0; s < num_succs; ++s) {
    succ_reach_probs[s] = new shared_ptr<double []>[2];
    for (int p = 0; p < 2; ++p) {
      succ_reach_probs[s][p].reset(new double[num_enc]);
    }
  }
  // Can happen when we are all-in.  Only succ is check.
  if (num_succs == 1) {
    for (int i = 0; i < num_ms_hole_card_pairs; ++i) {
      const Card *cards = hands_->Cards(i);
      Card hi = cards[0];
      Card lo = cards[1];
      int enc = hi * max_card1 + lo;
      for (int p = 0; p <= 1; ++p) {
	succ_reach_probs[0][p][enc] = reach_probs[p][enc];
      }
    }
    return succ_reach_probs;
  }
  int pa = node->PlayerActing();
  int nt = node->NonterminalID();
  int dsi = node->DefaultSuccIndex();
  unique_ptr<double []> probs(new double[num_succs]);
  for (int i = 0; i < num_ms_hole_card_pairs; ++i) {
    const Card *cards = hands_->Cards(i);
    Card hi = cards[0];
    Card lo = cards[1];
    int enc = hi * max_card1 + lo;
    int offset;
    int hcp;
    if (st == max_street && buckets.None(st)) {
      hcp = i;
    } else {
      hcp = ms_hcp_to_raw_hcp_[st][i];
    }
    if (buckets.None(st)) {
      offset = gbd * num_st_hole_card_pairs * num_succs + hcp * num_succs;
    } else {
      unsigned int h = ((unsigned int)gbd) * ((unsigned int)num_st_hole_card_pairs) + hcp;
      int b = buckets.Bucket(st, h);
      offset = b * num_succs;
    }
    sumprobs->RMProbs(st, pa, nt, offset, num_succs, dsi, probs.get());
    for (int s = 0; s < num_succs; ++s) {
      for (int p = 0; p <= 1; ++p) {
	if (p == pa) {
	  double prob = reach_probs[p][enc] * probs[s];
	  if (prob > 1.0 || prob < 0) {
	    fprintf(stderr, "OOB prob %f (%f * %f) enc %i st %i\n", prob, reach_probs[p][enc],
		    probs[s], enc, st);
	    OutputCard(hi);
	    printf(" ");
	    OutputCard(lo);
	    printf("\n");
	    const Card *board = BoardTree::Board(max_street, msbd_);
	    OutputFiveCards(board);
	    printf("\n");
	    exit(-1);
	  }
	  succ_reach_probs[s][p][enc] = prob;
	} else {
	  double prob = reach_probs[p][enc];
	  if (prob > 1.0 || prob < 0) {
	    fprintf(stderr, "OOB prob %f enc %i\n", prob, enc);
	    exit(-1);
	  }
	  succ_reach_probs[s][p][enc] = prob;
	}
      }
    }
  }
  
  return succ_reach_probs;
}

void Player::Nonterminal(Node *a_node, Node *b_node, const string &action_sequence,
			 shared_ptr<double []> *reach_probs) {
  int st = a_node->Street();
  int pa = a_node->PlayerActing();
  shared_ptr<double []> **succ_reach_probs;
  if (pa == b_pos_) {
    // This doesn't support multiplayer yet
    CFRValues *sumprobs;
    if (resolve_b_ && st == Game::MaxStreet()) {
      sumprobs = b_eg_cfr_->Sumprobs();
    } else {
      sumprobs = b_probs_.get();
    }
    const Buckets &buckets =
      (resolve_b_ && st >= resolve_st_) ? *b_subgame_buckets_ : *b_base_buckets_;
    succ_reach_probs = GetSuccReachProbs(b_node, b_boards_[st], buckets, sumprobs, reach_probs);
  } else {
    CFRValues *sumprobs;
    if (resolve_a_ && st == Game::MaxStreet()) {
      sumprobs = a_eg_cfr_->Sumprobs();
    } else {
      sumprobs = a_probs_.get();
    }
    const Buckets &buckets =
      (resolve_a_ && st >= resolve_st_) ? *a_subgame_buckets_ : *a_base_buckets_;
    succ_reach_probs = GetSuccReachProbs(a_node, a_boards_[st], buckets, sumprobs, reach_probs);
  }
  int num_succs = a_node->NumSuccs();
  for (int s = 0; s < num_succs; ++s) {
    string action = a_node->ActionName(s);
    Walk(a_node->IthSucc(s), b_node->IthSucc(s), action_sequence + action, succ_reach_probs[s], st);
  }
  for (int s = 0; s < num_succs; ++s) {
    delete [] succ_reach_probs[s];
  }
  delete [] succ_reach_probs;
}
 
void Player::Walk(Node *a_node, Node *b_node, const string &action_sequence,
		  shared_ptr<double []> *reach_probs, int last_st) {
  int st = a_node->Street();
  if (st > last_st && st == resolve_st_) {
    Node *next_a_node, *next_b_node;
    if (resolve_a_) {
      a_subtree_.reset(CreateSubtree(st, a_node->PlayerActing(), a_node->LastBetTo(), -1,
				     a_subgame_betting_abstraction_));
      int max_street = Game::MaxStreet();
      int root_bd;
      if (st == max_street) root_bd = msbd_;
      else                  root_bd = BoardTree::PredBoard(msbd_, st);
      struct timespec start, finish;
      clock_gettime(CLOCK_MONOTONIC, &start);
      a_eg_cfr_->SolveSubgame(a_subtree_.get(), root_bd, reach_probs, action_sequence,
			      resolve_hand_tree_.get(), nullptr, -1, true, num_subgame_its_);
      clock_gettime(CLOCK_MONOTONIC, &finish);
      resolving_secs_ += (finish.tv_sec - start.tv_sec);
      resolving_secs_ += (finish.tv_nsec - start.tv_nsec) / 1000000000.0;
      ++num_resolves_;
      next_a_node = a_subtree_->Root();
      for (int st1 = st; st1 <= max_street; ++st1) {
	int gbd;
	if (st1 == max_street) gbd = msbd_;
	else                   gbd = BoardTree::PredBoard(msbd_, st1);
	a_boards_[st1] = BoardTree::LocalIndex(st, root_bd, st1, gbd);
      }
    } else {
      next_a_node = a_node;
    }
    if (resolve_b_) {
      b_subtree_.reset(CreateSubtree(st, b_node->PlayerActing(), b_node->LastBetTo(), -1,
				     b_subgame_betting_abstraction_));
      int max_street = Game::MaxStreet();
      int root_bd;
      if (st == max_street) root_bd = msbd_;
      else                  root_bd = BoardTree::PredBoard(msbd_, st);
      struct timespec start, finish;
      clock_gettime(CLOCK_MONOTONIC, &start);
      b_eg_cfr_->SolveSubgame(b_subtree_.get(), root_bd, reach_probs, action_sequence,
			      resolve_hand_tree_.get(), nullptr, -1, true, num_subgame_its_);
      clock_gettime(CLOCK_MONOTONIC, &finish);
      resolving_secs_ += (finish.tv_sec - start.tv_sec);
      resolving_secs_ += (finish.tv_nsec - start.tv_nsec) / 1000000000.0;
      ++num_resolves_;
      next_b_node = b_subtree_->Root();
      for (int st1 = st; st1 <= max_street; ++st1) {
	int gbd;
	if (st1 == max_street) gbd = msbd_;
	else                   gbd = BoardTree::PredBoard(msbd_, st1);
	b_boards_[st1] = BoardTree::LocalIndex(st, root_bd, st1, gbd);
      }
    } else {
      next_b_node = b_node;
    }
    Walk(next_a_node, next_b_node, action_sequence, reach_probs, st);
    return;
  }
  if (a_node->Terminal()) {
    if (! b_node->Terminal()) {
      fprintf(stderr, "A terminal B nonterminal?!?\n");
      exit(-1);
    }
    if (a_node->Showdown()) {
      Showdown(a_node, b_node, reach_probs);
    } else {
      Fold(a_node, b_node, reach_probs);
    }
  } else {
    if (b_node->Terminal()) {
      fprintf(stderr, "A nonterminal B terminal?!?\n");
      exit(-1);
    }
    Nonterminal(a_node, b_node, action_sequence, reach_probs);
  }
}

void Player::ProcessMaxStreetBoard(int msbd) {
  int max_street = Game::MaxStreet();
  msbd_ = msbd;
  a_boards_[max_street] = msbd_;
  b_boards_[max_street] = msbd_;
  for (int st = 1; st < max_street; ++st) {
    int pbd = BoardTree::PredBoard(msbd_, st);
    a_boards_[st] = pbd;
    b_boards_[st] = pbd;
  }
  hand_tree_.reset(new HandTree(max_street, msbd_, max_street));
  if ((resolve_a_ || resolve_b_) && resolve_st_ < max_street) {
    resolve_hand_tree_.reset(new HandTree(resolve_st_, BoardTree::PredBoard(msbd_, resolve_st_),
					  max_street));
  } else {
    resolve_hand_tree_ = hand_tree_;
  }
  hands_ = hand_tree_->Hands(max_street, 0);
  int num_ms_hole_card_pairs = Game::NumHoleCardPairs(max_street);
  int num_board_cards = Game::NumBoardCards(max_street);
  int num_hole_cards = Game::NumCardsForStreet(0);
  int num_cards = num_board_cards + num_hole_cards;
  const Card *board = BoardTree::Board(max_street, msbd_);
  unique_ptr<Card []> cards(new Card[num_cards]);
  for (int i = 0; i < num_board_cards; ++i) {
    cards[num_hole_cards + i] = board[i];
  }
  for (int i = 0; i < num_ms_hole_card_pairs; ++i) {
    const Card *hole_cards = hands_->Cards(i);
    cards[0] = hole_cards[0];
    if (num_hole_cards == 2) cards[1] = hole_cards[1];
    for (int st = 0; st <= max_street; ++st) {
      ms_hcp_to_raw_hcp_[st][i] = HCPIndex(st, cards.get());
    }
  }

  // Maintaining reach probs for hole card pairs consistent with *max-street* board.
  int num_players = Game::NumPlayers();
  int max_card1 = Game::MaxCard() + 1;
  int num_enc = max_card1 * max_card1;
  unique_ptr<shared_ptr<double []> []> reach_probs(new shared_ptr<double []>[num_players]);
  for (int p = 0; p < num_players; ++p) {
    reach_probs[p].reset(new double[num_enc]);
    for (int i = 0; i < num_ms_hole_card_pairs; ++i) {
      const Card *cards = hands_->Cards(i);
      Card hi = cards[0];
      Card lo = cards[1];
      int enc = hi * max_card1 + lo;
      reach_probs[p][enc] = 1.0;
    }
  }
  for (b_pos_ = 0; b_pos_ < num_players; ++b_pos_) {
    Walk(a_betting_tree_->Root(), b_betting_tree_->Root(), "x", reach_probs.get(), 0);
  }
}

void Player::Go(int num_sampled_max_street_boards) {
  num_resolves_ = 0;
  resolving_secs_ = 0;
  int max_street = Game::MaxStreet();
  int num_max_street_boards = BoardTree::NumBoards(max_street);
  if (num_sampled_max_street_boards == 0 ||
      num_sampled_max_street_boards > num_max_street_boards) {
    num_sampled_max_street_boards = num_max_street_boards;
  }

  if (num_sampled_max_street_boards == num_max_street_boards) {
    fprintf(stderr, "Processing all max street boards\n");
    for (int bd = 0; bd < num_max_street_boards; ++bd) {
      num_samples_ = BoardTree::BoardCount(max_street, bd);
      ProcessMaxStreetBoard(bd);
    }
  } else {
    unique_ptr<int []> max_street_board_samples(new int[num_max_street_boards]);
    for (int bd = 0; bd < num_max_street_boards; ++bd) max_street_board_samples[bd] = 0;
    struct drand48_data rand_buf;
    struct timeval time; 
    gettimeofday(&time, NULL);
    srand48_r((time.tv_sec * 1000) + (time.tv_usec / 1000), &rand_buf);
    vector< pair<double, int> > v;
    for (int bd = 0; bd < num_max_street_boards; ++bd) {
      int board_count = BoardTree::BoardCount(max_street, bd);
      for (int i = 0; i < board_count; ++i) {
	double r;
	drand48_r(&rand_buf, &r);
	v.push_back(std::make_pair(r, bd));
      }
    }
    std::sort(v.begin(), v.end());
    for (int i = 0; i < num_sampled_max_street_boards; ++i) {
      int bd = v[i].second;
      ++max_street_board_samples[bd];
    }

    for (int bd = 0; bd < num_max_street_boards; ++bd) {
      num_samples_ = max_street_board_samples[bd];
      if (num_samples_ == 0) continue;
      ProcessMaxStreetBoard(bd);
    }
  }

  double avg_b_outcome = sum_b_outcomes_ / sum_weights_;
  // avg_b_outcome is in units of the small blind
  double b_mbb_g = (avg_b_outcome / 2.0) * 1000.0;
  fprintf(stderr, "Avg B outcome: %f (%.1f mbb/g)\n", avg_b_outcome, b_mbb_g);
  double avg_p1_outcome = sum_p1_outcomes_ / sum_weights_;
  double p1_mbb_g = (avg_p1_outcome / 2.0) * 1000.0;
  fprintf(stderr, "Avg P1 outcome: %f (%.1f mbb/g)\n", avg_p1_outcome, p1_mbb_g);
  fprintf(stderr, "%.1f secs spent resolving\n", resolving_secs_);
  if (num_resolves_ > 0) {
    fprintf(stderr, "Avg %.2f secs per resolve (%i resolves)\n", resolving_secs_ / num_resolves_,
	    num_resolves_);
  }
}

static void Usage(const char *prog_name) {
  fprintf(stderr, "USAGE: %s <game params> <A card params> <B card params> "
	  "<A betting abstraction params> <B betting abstraction params> <A CFR params> "
	  "<B CFR params> <A it> <B it> <num sampled max street boards> <resolve st> <resolve A> "
	  "<resolve B> (<A resolve card params> <A resolve betting params> <A resolve CFR config>) "
	  "(<B resolve card params> <B resolve betting params> <B resolve CFR config>)\n",
	  prog_name);
  fprintf(stderr, "\n");
  fprintf(stderr, "Specify 0 for <num sampled max street boards> to not sample\n");
  fprintf(stderr, "<resolve A> and <resolve B> must be \"true\" or \"false\"\n");
  exit(-1);
}

int main(int argc, char *argv[]) {
  if (argc != 14 && argc != 17 && argc != 20) Usage(argv[0]);
  Files::Init();
  unique_ptr<Params> game_params = CreateGameParams();
  game_params->ReadFromFile(argv[1]);
  Game::Initialize(*game_params);
  unique_ptr<Params> a_card_params = CreateCardAbstractionParams();
  a_card_params->ReadFromFile(argv[2]);
  unique_ptr<CardAbstraction>
    a_card_abstraction(new CardAbstraction(*a_card_params));
  unique_ptr<Params> b_card_params = CreateCardAbstractionParams();
  b_card_params->ReadFromFile(argv[3]);
  unique_ptr<CardAbstraction>
    b_card_abstraction(new CardAbstraction(*b_card_params));
  unique_ptr<Params> a_betting_params = CreateBettingAbstractionParams();
  a_betting_params->ReadFromFile(argv[4]);
  unique_ptr<BettingAbstraction>
    a_betting_abstraction(new BettingAbstraction(*a_betting_params));
  unique_ptr<Params> b_betting_params = CreateBettingAbstractionParams();
  b_betting_params->ReadFromFile(argv[5]);
  unique_ptr<BettingAbstraction>
    b_betting_abstraction(new BettingAbstraction(*b_betting_params));
  unique_ptr<Params> a_cfr_params = CreateCFRParams();
  a_cfr_params->ReadFromFile(argv[6]);
  unique_ptr<CFRConfig>
    a_cfr_config(new CFRConfig(*a_cfr_params));
  unique_ptr<Params> b_cfr_params = CreateCFRParams();
  b_cfr_params->ReadFromFile(argv[7]);
  unique_ptr<CFRConfig>
    b_cfr_config(new CFRConfig(*b_cfr_params));

  int a_it, b_it, num_sampled_max_street_boards, resolve_st;
  if (sscanf(argv[8], "%i", &a_it) != 1)                           Usage(argv[0]);
  if (sscanf(argv[9], "%i", &b_it) != 1)                           Usage(argv[0]);
  if (sscanf(argv[10], "%i", &num_sampled_max_street_boards) != 1) Usage(argv[0]);
  if (sscanf(argv[11], "%i", &resolve_st) != 1)                    Usage(argv[0]);

  bool resolve_a = false;
  bool resolve_b = false;
  string ra = argv[12];
  if (ra == "true")       resolve_a = true;
  else if (ra == "false") resolve_a = false;
  else                    Usage(argv[0]);
  string rb = argv[13];
  if (rb == "true")       resolve_b = true;
  else if (rb == "false") resolve_b = false;
  else                    Usage(argv[0]);
  if (! resolve_a && ! resolve_b && resolve_st != -1) {
    fprintf(stderr, "resolve_st should be -1 if not resolving either A or B\n");
    exit(-1);
  }

  if (resolve_a && resolve_b && argc != 20)     Usage(argv[0]);
  if (resolve_a && ! resolve_b && argc != 17)   Usage(argv[0]);
  if (! resolve_a && resolve_b && argc != 17)   Usage(argv[0]);
  if (! resolve_a && ! resolve_b && argc != 14) Usage(argv[0]);

  unique_ptr<CardAbstraction> a_subgame_card_abstraction, b_subgame_card_abstraction;
  unique_ptr<BettingAbstraction> a_subgame_betting_abstraction, b_subgame_betting_abstraction;
  unique_ptr<CFRConfig> a_subgame_cfr_config, b_subgame_cfr_config;
  if (resolve_a) {
    unique_ptr<Params> subgame_card_params = CreateCardAbstractionParams();
    subgame_card_params->ReadFromFile(argv[14]);
    a_subgame_card_abstraction.reset(new CardAbstraction(*subgame_card_params));
    unique_ptr<Params> subgame_betting_params = CreateBettingAbstractionParams();
    subgame_betting_params->ReadFromFile(argv[15]);
    a_subgame_betting_abstraction.reset(new BettingAbstraction(*subgame_betting_params));
    unique_ptr<Params> subgame_cfr_params = CreateCFRParams();
    subgame_cfr_params->ReadFromFile(argv[16]);
    a_subgame_cfr_config.reset(new CFRConfig(*subgame_cfr_params));
  }
  if (resolve_b) {
    int a = resolve_a ? 17 : 14;
    unique_ptr<Params> subgame_card_params = CreateCardAbstractionParams();
    subgame_card_params->ReadFromFile(argv[a]);
    b_subgame_card_abstraction.reset(new CardAbstraction(*subgame_card_params));
    unique_ptr<Params> subgame_betting_params = CreateBettingAbstractionParams();
    subgame_betting_params->ReadFromFile(argv[a+1]);
    b_subgame_betting_abstraction.reset(new BettingAbstraction(*subgame_betting_params));
    unique_ptr<Params> subgame_cfr_params = CreateCFRParams();
    subgame_cfr_params->ReadFromFile(argv[a+2]);
    b_subgame_cfr_config.reset(new CFRConfig(*subgame_cfr_params));
  }

  Player player(*a_betting_abstraction, *b_betting_abstraction, *a_card_abstraction,
		*b_card_abstraction, *a_cfr_config, *b_cfr_config, a_it, b_it, resolve_st,
		resolve_a, resolve_b, *a_subgame_card_abstraction, *a_subgame_betting_abstraction,
		*a_subgame_cfr_config, *b_subgame_card_abstraction, *b_subgame_betting_abstraction,
		*b_subgame_cfr_config);
  player.Go(num_sampled_max_street_boards);
}
