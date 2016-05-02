/******************************************************************************
*
* Author: Christopher R. Aberger
*
******************************************************************************/
#ifndef _TRIEBUFFER_H_
#define _TRIEBUFFER_H_

#include "utils/utils.hpp"
#include "Vector.hpp"

/*
A Dense Buffer. 
*/

template<class A>
struct TrieBuffer{
  size_t num_columns;
  std::vector<std::vector<Vector<BLASVector,void*,ParMemoryBuffer>>> buffers;
  A* anno;
  size_t num_anno;
  bool is_blas_query;
  size_t buffer_block_size;

  static size_t set_anno(const size_t nc,const size_t buffer_block_size_in)
  {
    return pow(buffer_block_size_in,nc);
  }

  TrieBuffer<A>(
    const bool is_blas_query_in,
    ParMemoryBuffer* memoryBuffersIn,
    const size_t num_columns_in,
    const size_t buffer_block_size_in){
    buffer_block_size = buffer_block_size_in;
    is_blas_query = is_blas_query_in;
    num_columns = num_columns_in;

    ParMemoryBuffer* memoryBuffers = new ParMemoryBuffer("",2);
    num_anno = TrieBuffer<A>::set_anno(num_columns,buffer_block_size_in);
    if(is_blas_query){
      //tricky but we want these to point to output trie's anno
      memoryBuffers->anno = memoryBuffersIn->anno;
      anno = (A*)memoryBuffersIn->anno->get_address(0);
    } else{
      anno = (A*)memoryBuffers->anno->get_next(num_anno*sizeof(A));
    }

    const size_t num_words = (buffer_block_size >> ADDRESS_BITS_PER_WORD)+1;
    const size_t bytes_per_level =
      sizeof(size_t)+
      sizeof(Meta)+
      num_words*sizeof(uint64_t);

    for(size_t i = 0; i < num_columns; i++){
      const size_t num = pow(buffer_block_size,i);
      std::vector<Vector<BLASVector,void*,ParMemoryBuffer>> b;
      for(size_t j = 0; j < num; j++){
        Vector<BLASVector,void*,ParMemoryBuffer> head(
          NUM_THREADS,
          memoryBuffers,
          bytes_per_level
        );
        Meta* m = head.get_meta();
        m->start = 0;
        m->end = 0;
        m->cardinality = 0;
        m->type = type::BITSET;
        b.push_back(head);
      }
      buffers.push_back(b);
    }
  };

  /*
  Copy Vector.
  */
  inline Vector<EHVector,A,ParMemoryBuffer> sparsify_vector(
    const size_t tid,
    ParMemoryBuffer* memoryBuffer){

    Vector<BLASVector,float,ParMemoryBuffer> copy_vec = this->at<float>(0,0);
    const size_t anno_offset = *(size_t*)copy_vec.get_this();

    if(copy_vec.get_meta()->cardinality > MIN_BITSET_LENGTH){
      const float density = ((float)copy_vec.get_meta()->cardinality)/(copy_vec.get_meta()->end-copy_vec.get_meta()->start);
      if(density >= VECTOR_DENSITY_THRESHOLD){
        Vector<EHVector,A,ParMemoryBuffer> cur(
          tid,
          memoryBuffer,
          (uint8_t*)copy_vec.get_meta(),
          copy_vec.get_num_index_bytes()-sizeof(size_t),
          anno+anno_offset,
          copy_vec.get_num_annotation_bytes<A>()
        );
        return cur;
      }
    }

    //over allocate (counts not guaranteed to be precise from union)
    Meta* copy_meta = copy_vec.get_meta();
    const size_t num_elems =(copy_meta->end-copy_meta->start);
    const size_t num_bytes = sizeof(Meta)+ 
      num_elems*(sizeof(uint32_t)+sizeof(A));
    
    size_t bytes_used = sizeof(Meta);    
    const size_t index = memoryBuffer->get_offset(tid);

    BufferIndex bufferIndex(tid,index);
    Meta* meta = (Meta*)(memoryBuffer->get_next(tid,num_bytes));
    meta->cardinality = 0;
    meta->start = 0;
    meta->end = 0;
    meta->type = type::UINTEGER;

    uint32_t* uint_buffer = (uint32_t*)(((uint8_t*)meta)+sizeof(Meta));
    A* anno_buffer = (A*)(((uint8_t*)uint_buffer)+(num_elems*sizeof(uint32_t)));
    const A* start_anno_buffer = (A*)(((uint8_t*)uint_buffer)+(num_elems*sizeof(uint32_t)));

    bool first = true;
    copy_vec.foreach([&](const uint32_t i, const uint32_t d, const A anno){
      (void) i;
      if(first)
        meta->start = d;
      first = false;
      *uint_buffer = d;
      bytes_used += (sizeof(uint32_t)+sizeof(A));
      meta->cardinality++;
      meta->end = d;
      *anno_buffer = anno;
      uint_buffer++;
      anno_buffer++;
    });
    
    memcpy((void*)uint_buffer,(void*)start_anno_buffer,(meta->cardinality*sizeof(A)));
    assert(num_bytes >= bytes_used);
    memoryBuffer->roll_back(tid,num_bytes-bytes_used);

    Vector<EHVector,A,ParMemoryBuffer> cur(memoryBuffer,bufferIndex);
    return cur;
  }

  /*
  Copy a single level buffer over.
  */
  inline Vector<BLASVector,A,ParMemoryBuffer> copy_vector(
    const size_t tid,
    ParMemoryBuffer* memoryBuffer){

    Vector<BLASVector,void*,ParMemoryBuffer> copy_vec = buffers.at(0).at(0);
    const size_t anno_offset = *(size_t*)copy_vec.get_this();

    Vector<BLASVector,A,ParMemoryBuffer> cur(
      tid,
      memoryBuffer,
      copy_vec.get_this(),
      copy_vec.get_num_index_bytes(),
      anno+anno_offset,
      copy_vec.get_num_annotation_bytes<A>()
    );
    return cur;
  }

  /*
  Copy a two level buffer over (FIXME: generalize to any dimension > 1)
  */
  inline Vector<EHVector,BufferIndex,ParMemoryBuffer> copy_matrix(
    const size_t tid,
    ParMemoryBuffer* memoryBuffer){

    Vector<BLASVector,void*,ParMemoryBuffer> copy_vec = buffers.at(0).at(0);
    Vector<EHVector,BufferIndex,ParMemoryBuffer> head(
      tid,
      memoryBuffer,
      copy_vec.get_meta(),
      copy_vec.get_num_index_bytes()-sizeof(size_t),
      copy_vec. template get_num_annotation_bytes<BufferIndex>()
    );

    head.foreach_index([&](const uint32_t index, const uint32_t data){
      copy_vec = buffers.at(1).at(data%buffer_block_size);
      const size_t anno_offset = *(size_t*)copy_vec.get_this();
      Vector<BLASVector,A,ParMemoryBuffer> cur(
        tid,
        memoryBuffer,
        (uint8_t*)copy_vec.get_this(),
        copy_vec.get_num_index_bytes(),
        anno+anno_offset,
        copy_vec.get_num_annotation_bytes<A>()
      );
      head.set(index,data,cur.bufferIndex);
    });
    return head;
  };

  inline void zero(
    const std::vector<size_t>& dimensions,
    const std::vector<size_t>& offset){
    for(size_t i = 0; i < num_columns; i++){
      const size_t num = pow(buffer_block_size,i);
      const size_t num_words = (buffer_block_size >> ADDRESS_BITS_PER_WORD)+1;

      size_t anno_offset2 = 0;
      for(size_t j = i; j > 0; j--){
        anno_offset2 += dimensions.at(j)*buffer_block_size*offset.at(j-1);
      }
      for(size_t j = 0; j < num; j++){
        Vector<BLASVector,void*,ParMemoryBuffer> cur = 
          buffers.at(i).at(j);
        uint8_t *data = cur.get_index_data();
        memset(data,0,num_words*sizeof(uint64_t));
        Meta* m = cur.get_meta();
        m->start = (offset.at(i)*buffer_block_size);
        m->end = (offset.at(i)*buffer_block_size)+buffer_block_size-1;
        m->cardinality = 0;
        m->type = type::BITSET;
        if(is_blas_query){
          const size_t anno_offset = anno_offset2+
            (offset.at(i)*buffer_block_size)+
            (j*dimensions.at(i));
          *(size_t*)cur.get_this() = anno_offset;
          memset((anno+anno_offset),0,buffer_block_size*sizeof(A));
        } else{
          *(size_t*)cur.get_this() = 0;
          memset(anno,0,buffer_block_size*sizeof(A));
        }
      }
    }
  };

  inline void print(){
    A* start = anno;
    for(size_t j = 0; j < buffer_block_size; j++){
      //if(start[j] != 0){
        std::cout << "j: " << j << " " << start[j] << std::endl;
      //}
    }
  };

  template<class R>
  inline Vector<BLASVector,R,ParMemoryBuffer> at(
    const size_t column, 
    const size_t index){

    Vector<BLASVector,void*,ParMemoryBuffer> tmp_j = 
      buffers.at(column).at(index%buffer_block_size);
    Vector<BLASVector,R,ParMemoryBuffer> tmp_jj(
      tmp_j.memoryBuffer,
      tmp_j.bufferIndex);

    return tmp_jj;
  }
};

/*
Copy a single level buffer over.
*/
template<> 
inline Vector<BLASVector,void*,ParMemoryBuffer> TrieBuffer<void*>::copy_vector(
  const size_t tid,
  ParMemoryBuffer* memoryBuffer){

  Vector<BLASVector,void*,ParMemoryBuffer> copy_vec = buffers.at(0).at(0);
  const size_t anno_offset = *(size_t*)copy_vec.get_this();
  Vector<BLASVector,void*,ParMemoryBuffer> cur(
    tid,
    memoryBuffer,
    copy_vec.get_this(),
    copy_vec.get_num_index_bytes(),
    anno+anno_offset,
    0
  );
  return cur;
}

/*
Copy a two-level buffer over.
*/
template<> 
inline Vector<EHVector,BufferIndex,ParMemoryBuffer> TrieBuffer<void*>::copy_matrix(
  const size_t tid,
  ParMemoryBuffer* memoryBuffer){

  Vector<BLASVector,void*,ParMemoryBuffer> copy_vec = buffers.at(0).at(0);
  Vector<EHVector,BufferIndex,ParMemoryBuffer> head(
    tid,
    memoryBuffer,
    (uint8_t*)copy_vec.get_meta(),
    copy_vec.get_num_index_bytes()-sizeof(size_t),
    copy_vec. template get_num_annotation_bytes<BufferIndex>()
  );

  head.foreach_index([&](const uint32_t index, const uint32_t data){
    copy_vec = buffers.at(1).at(data%buffer_block_size);
    Vector<BLASVector,void*,ParMemoryBuffer> cur(
      tid,
      memoryBuffer,
      (uint8_t*)buffers.at(1).at(data%buffer_block_size).get_this(),
      buffers.at(1).at(data%buffer_block_size).get_num_index_bytes(),
      0
    );
    head.set(index,data,cur.bufferIndex);
  });
  return head;
}

template<>
size_t TrieBuffer<void*>::set_anno(const size_t nc,const size_t buffer_block_size_in)
{
  (void) nc;
  return 0;
}

template<>
inline void TrieBuffer<void*>::zero(
  const std::vector<size_t>& dimensions,
  const std::vector<size_t>& offset){
  for(size_t i = 0; i < num_columns; i++){
    const size_t num = pow(buffer_block_size,i);
    const size_t num_words = (buffer_block_size >> ADDRESS_BITS_PER_WORD)+1;

    size_t anno_offset2 = 0;
    for(size_t j = i; j > 0; j--){
      anno_offset2 += dimensions.at(j)*buffer_block_size*offset.at(j-1);
    }

    for(size_t j = 0; j < num; j++){
      Vector<BLASVector,void*,ParMemoryBuffer> cur = 
        buffers.at(i).at(j);
      uint8_t *data = cur.get_index_data();
      memset(data,0,num_words*sizeof(uint64_t));
      Meta* m = cur.get_meta();
      m->start = (offset.at(i)*buffer_block_size);
      m->end = (offset.at(i)*buffer_block_size)+buffer_block_size-1;
      m->cardinality = 0;
      m->type = type::BITSET;
      const size_t anno_offset = anno_offset2+
        (offset.at(i)*buffer_block_size)+
        (j*dimensions.at(i));
      *(size_t*)cur.get_this() = anno_offset;
    }
  }
}

#endif