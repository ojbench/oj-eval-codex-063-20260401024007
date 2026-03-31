#ifndef BPT_MEMORYRIVER_HPP
#define BPT_MEMORYRIVER_HPP

#include <fstream>
#include <string>

using std::string;
using std::fstream;
using std::ifstream;
using std::ofstream;

// MemoryRiver implements a simple file-backed storage with space reclamation via a free list.
// Header layout:
// - The first `info_len` ints are reserved for user info via get_info/write_info (1-based indexing).
// - We implicitly add two extra ints for internal bookkeeping placed right after the user header:
//   1) tail_offset: byte offset of the next available position for append writes
//   2) free_head: head byte offset of the free list (0 means empty)
// Indices returned by write() are byte offsets of the object within the file.

template<class T, int info_len = 2>
class MemoryRiver {
private:
    static constexpr int internal_info_len = 2; // tail_offset and free_head

    fstream file;
    string file_name;
    int sizeofT = sizeof(T);

    // Helper: total number of header ints (user + internal)
    static constexpr int total_header_ints() { return info_len + internal_info_len; }

    // Helper: header size in bytes
    static constexpr std::streamoff header_bytes() {
        return static_cast<std::streamoff>(total_header_ints()) * static_cast<std::streamoff>(sizeof(int));
    }

    // Helper: open file in read/write binary mode
    void open_rw() {
        file.open(file_name, std::ios::in | std::ios::out | std::ios::binary);
        if (!file.is_open()) {
            // Create file if not exists
            file.clear();
            file.open(file_name, std::ios::out | std::ios::binary);
            file.close();
            file.open(file_name, std::ios::in | std::ios::out | std::ios::binary);
        }
    }

    // Internal info accessors (1-based within internal section)
    void get_internal(int &tmp, int n) {
        // n in [1, internal_info_len]
        std::streamoff off = static_cast<std::streamoff>(info_len + n - 1) * static_cast<std::streamoff>(sizeof(int));
        file.seekg(off, std::ios::beg);
        file.read(reinterpret_cast<char *>(&tmp), sizeof(int));
    }

    void write_internal(int tmp, int n) {
        std::streamoff off = static_cast<std::streamoff>(info_len + n - 1) * static_cast<std::streamoff>(sizeof(int));
        file.seekp(off, std::ios::beg);
        file.write(reinterpret_cast<char *>(&tmp), sizeof(int));
        file.flush();
    }

    // Initialize internal bookkeeping if file is shorter than expected
    void ensure_internal_initialized() {
        // Determine file size
        file.seekg(0, std::ios::end);
        std::streamoff sz = file.tellg();
        // If file shorter than full header, extend and initialize zeros
        std::streamoff need = header_bytes();
        if (sz < need) {
            file.clear();
            file.seekp(0, std::ios::end);
            int zero = 0;
            // Write zeros to reach full header
            for (std::streamoff i = sz / sizeof(int); i < total_header_ints(); ++i) {
                file.write(reinterpret_cast<char *>(&zero), sizeof(int));
            }
            file.flush();
            // Set initial tail offset to header size and free_head to 0
            int tail = static_cast<int>(need);
            write_internal(tail, 1);
            int free_head = 0;
            write_internal(free_head, 2);
        } else {
            // If header exists but tail is zero (uninitialized legacy), set defaults
            int tail = 0;
            int free_head = 0;
            file.clear();
            get_internal(tail, 1);
            get_internal(free_head, 2);
            if (tail == 0) {
                tail = static_cast<int>(need);
                write_internal(tail, 1);
            }
        }
        file.clear();
    }

public:
    MemoryRiver() = default;

    explicit MemoryRiver(const string& file_name) : file_name(file_name) {}

    void initialise(string FN = "") {
        if (FN != "") file_name = FN;
        // Truncate and write zeros for user + internal header
        file.open(file_name, std::ios::out | std::ios::binary | std::ios::trunc);
        int tmp = 0;
        for (int i = 0; i < total_header_ints(); ++i)
            file.write(reinterpret_cast<char *>(&tmp), sizeof(int));
        file.close();
        // Set internal defaults: tail at end of header, free_head = 0
        open_rw();
        int tail = static_cast<int>(header_bytes());
        write_internal(tail, 1);
        int free_head = 0;
        write_internal(free_head, 2);
        file.close();
    }

    // Read the n-th (1-based) user info int into tmp
    void get_info(int &tmp, int n) {
        if (n > info_len) return;
        open_rw();
        // Map n to correct offset within user header region
        std::streamoff off = static_cast<std::streamoff>(n - 1) * static_cast<std::streamoff>(sizeof(int));
        file.seekg(off, std::ios::beg);
        file.read(reinterpret_cast<char *>(&tmp), sizeof(int));
        file.close();
    }

    // Write tmp into the n-th (1-based) user info int
    void write_info(int tmp, int n) {
        if (n > info_len) return;
        open_rw();
        std::streamoff off = static_cast<std::streamoff>(n - 1) * static_cast<std::streamoff>(sizeof(int));
        file.seekp(off, std::ios::beg);
        file.write(reinterpret_cast<char *>(&tmp), sizeof(int));
        file.flush();
        file.close();
    }

    // Write object t to file and return its index (byte offset)
    int write(T &t) {
        open_rw();
        ensure_internal_initialized();

        int free_head = 0;
        get_internal(free_head, 2);

        int index = 0;
        if (free_head != 0) {
            // Pop from free list
            index = free_head;
            int next_free = 0;
            // Read next free pointer stored at freed block start
            file.seekg(static_cast<std::streamoff>(index), std::ios::beg);
            file.read(reinterpret_cast<char *>(&next_free), sizeof(int));
            // Update free_head
            write_internal(next_free, 2);
        } else {
            // Append at tail
            int tail = 0;
            get_internal(tail, 1);
            index = tail;
            tail += sizeofT;
            write_internal(tail, 1);
        }

        // Write T at index
        file.seekp(static_cast<std::streamoff>(index), std::ios::beg);
        file.write(reinterpret_cast<char *>(&t), sizeof(T));
        file.flush();
        file.close();
        return index;
    }

    // Update object at given index with t
    void update(T &t, const int index) {
        open_rw();
        // Assume index is valid and produced by write()
        file.seekp(static_cast<std::streamoff>(index), std::ios::beg);
        file.write(reinterpret_cast<char *>(&t), sizeof(T));
        file.flush();
        file.close();
    }

    // Read object at given index into t
    void read(T &t, const int index) {
        open_rw();
        file.seekg(static_cast<std::streamoff>(index), std::ios::beg);
        file.read(reinterpret_cast<char *>(&t), sizeof(T));
        file.close();
    }

    // Delete object at given index and reclaim space by pushing onto free list
    void Delete(int index) {
        open_rw();
        ensure_internal_initialized();
        int free_head = 0;
        get_internal(free_head, 2);
        // Overwrite the beginning of this block with the current free_head pointer
        file.seekp(static_cast<std::streamoff>(index), std::ios::beg);
        file.write(reinterpret_cast<char *>(&free_head), sizeof(int));
        file.flush();
        // Set new free_head to this index
        write_internal(index, 2);
        file.close();
    }
};

#endif //BPT_MEMORYRIVER_HPP
