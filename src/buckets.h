#ifndef _BUCKETS_H_
#define _BUCKETS_H_

class CardAbstraction;

class Buckets {
public:
  Buckets(const CardAbstraction &ca, bool numb_only);
  ~Buckets(void);
  bool None(int st) const {return none_[st];}
  // Use an unsigned int for hands.  For full holdem, the number of hands exceeds kMaxInt.
  int Bucket(int st, unsigned int h) const {
    if (short_buckets_[st]) {
      return (int)short_buckets_[st][h];
    } else {
      return int_buckets_[st][h];
    }
  }
  const int *NumBuckets(void) const {return num_buckets_;}
  int NumBuckets(int st) const {return num_buckets_[st];}
private:
  bool *none_;
  unsigned short **short_buckets_;
  int **int_buckets_;
  int *num_buckets_;
};

#endif