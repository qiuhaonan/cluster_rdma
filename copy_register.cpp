#include <iostream>
#include <chrono>
#include <vector>
#include <infiniband/verbs.h>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <cstdlib>
#include <cassert>
#include <sys/time.h>
#include <tuple>
#include <iomanip>

using namespace std;

enum RecordFields {SIZE, CPY, REG, DEG, REG_DEG, MLOCK};
enum Type {SMALL, BIG};

class infiniband_device
{
  public:
    struct ibv_context *ib_ctx;
    struct ibv_pd *ib_pd;
    string device_name;
    int    mem_size;
    int    iteration;
    void*  srcbuf;
    void*  dstbuf;
    vector<tuple<int, double, double, double, double, double>> small_page;
    vector<tuple<int, double, double, double, double, double>> big_page;
    Type   test_type;

    infiniband_device() : ib_ctx(nullptr), ib_pd(nullptr), device_name("mlx5_0"), mem_size(1024 * 1024 * 1024), srcbuf(nullptr), dstbuf(nullptr), test_type(SMALL), iteration(10) {}
    ~infiniband_device()
    {
        if(ib_pd)
        {
            ibv_dealloc_pd(ib_pd);
        }
        if(ib_ctx)
        {
            ibv_close_device(ib_ctx);
        }
    }
    int init_device();
    void allocate_buffer_using_small_page();
    void allocate_buffer_using_big_page();
    void free_buffer_using_small_page();
    void free_buffer_using_big_page();
    void run_test();
    int random_offset(int, int);
    void output_big_page_test();
    void output_small_page_test();
};

int infiniband_device::random_offset(int mem_size, int io_size) 
{
    struct timeval now; 
    gettimeofday(&now, NULL);
    srand(now.tv_sec + now.tv_usec);
    int offset = rand();
    offset = (offset % (mem_size - io_size));
    //return offset;
    return 0;
}

int
infiniband_device::init_device()
{
    struct ibv_device **dev_list;
    struct ibv_device *ib_dev;

    dev_list = ibv_get_device_list(NULL);
    if (!dev_list)
    {
        cerr << "Failed to get IB devices list" << endl;
        return 1;
    }

    if (!device_name.empty())
    {
        ib_dev = *dev_list;
        if (!ib_dev)
        {
            cerr << "No IB devices found" << endl;
            return 1;
        }
    }
    else
    {
        int i;
        for (i = 0; dev_list[i]; ++i)
        {
            string iter_name(ibv_get_device_name(dev_list[i]));
            if (iter_name == device_name)
                break;
        }

        ib_dev = dev_list[i];
        if (!ib_dev)
        {
            cerr << "IB device " << device_name << " not found " << endl;
            return 1;
        }
    }

    ib_ctx = ibv_open_device(ib_dev);
    if (!ib_ctx)
    {
        cerr << "Couldn't get context for " << ibv_get_device_name(ib_dev) << endl;
        return 1;
    }

    ib_pd = ibv_alloc_pd(ib_ctx);
    if (!ib_pd)
    {
        cerr << "failed to allocate pd" << endl;
        return 1;
    }
    return 0;
}

void infiniband_device::allocate_buffer_using_small_page()
{
    srcbuf = malloc(mem_size);
    dstbuf = malloc(mem_size);
    memset(srcbuf, 'a', mem_size);
    memset(dstbuf, 'a', mem_size);
    test_type = SMALL;
}

void infiniband_device::free_buffer_using_small_page()
{
    free(srcbuf);
    free(dstbuf);
}

void infiniband_device::free_buffer_using_big_page()
{
    munmap(srcbuf, mem_size);
    munmap(dstbuf, mem_size);
}

void infiniband_device::allocate_buffer_using_big_page()
{
    mem_size = ((mem_size / (2 * 1024 * 1024)) + 1) * (2 * 1024 * 1024);
    cout << "Allocate " << mem_size / (2 * 1024 * 1024) << " x 2MB Page" << endl;
    srcbuf = mmap(nullptr, mem_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    dstbuf = mmap(nullptr, mem_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    memset(srcbuf, 'a', mem_size);
    memset(dstbuf, 'a', mem_size);
    test_type = BIG;
}

void infiniband_device::run_test()
{
    for (uint64_t i = 8; i <= (mem_size / 2); i = i * 2)
    {
        double cpy_sum = 0, reg_sum = 0, deg_sum = 0, mlock_sum = 0;
        // choose addr 
        auto offset = random_offset(mem_size, i);
        
        for (int j = 0; j < iteration; ++j)
        {
            void* srcptr = (void*)(((char*)srcbuf) + offset);
            void* dstptr = (void*)(((char*)dstbuf) + offset);
            memset(srcptr, 'a', i);
            auto start = std::chrono::high_resolution_clock::now();
            memcpy(dstptr, srcptr, i);
            auto end = std::chrono::high_resolution_clock::now();
            cpy_sum += (std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());    
            
            void* ptr = (void*)(((char*)srcbuf) + offset);
            start = std::chrono::high_resolution_clock::now();
            ibv_mr *mr = ibv_reg_mr(ib_pd, ptr, i, IBV_ACCESS_LOCAL_WRITE);
            end = std::chrono::high_resolution_clock::now();
            reg_sum += (std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());

            start = std::chrono::high_resolution_clock::now();
            ibv_dereg_mr(mr);
            end = std::chrono::high_resolution_clock::now();
            deg_sum += (std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());

            void* mptr = (void*)(((char*)srcbuf) + offset);
            start = std::chrono::high_resolution_clock::now();
            assert(mlock(mptr, i) == 0);
            end = std::chrono::high_resolution_clock::now();
            mlock_sum += (std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
            assert(munlock(mptr, i) == 0); 
        }

        if (test_type == SMALL)
            small_page.push_back(make_tuple(i, cpy_sum/iteration, reg_sum/iteration, deg_sum/iteration, (reg_sum + deg_sum)/iteration, mlock_sum/iteration));
        else 
            big_page.push_back(make_tuple(i, cpy_sum/iteration, reg_sum/iteration, deg_sum/iteration, (reg_sum + deg_sum)/iteration, mlock_sum/iteration));
    }
}

void infiniband_device::output_small_page_test()
{
    int num = small_page.size();
    cout << setw(10) << "Type" << setw(10) << "Bytes" << setw(10) << "CPY(us)" << setw(10) << "REG(us)" << setw(10) << "DEG(us)" << setw(10) << "REDEG(us)" << setw(10) << "MLOCK(us)" << endl;
    for(int i = 0; i<num; ++i)
    {
        cout << setw(10) << "4K Page" << setw(10) << std::get<SIZE>(small_page[i]) << setw(10) << std::get<CPY>(small_page[i]) << setw(10) << std::get<REG>(small_page[i]) << setw(10) << std::get<DEG>(small_page[i]) << setw(10) << std::get<REG_DEG>(small_page[i]) << setw(10) << std::get<MLOCK>(small_page[i]) << endl;
    }
}

void infiniband_device::output_big_page_test()
{
    int num = big_page.size();
    cout << setw(10) << "Type" << setw(10) << "Bytes" << setw(10) << "CPY(us)" << setw(10) << "REG(us)" << setw(10) << "DEG(us)" << setw(10) << "REDEG(us)" << setw(10) << "MLOCK(us)" << endl;
    for(int i = 0; i<num; ++i)
    {
        cout << setw(10) << "2M Page" << setw(10) << std::get<SIZE>(big_page[i]) << setw(10) << std::get<CPY>(big_page[i]) << setw(10) << std::get<REG>(big_page[i]) << setw(10) << std::get<DEG>(big_page[i]) << setw(10) << std::get<REG_DEG>(big_page[i]) << setw(10) << std::get<MLOCK>(big_page[i])<< endl;
    }
}

int main(int argc, char *argv[])
{
    infiniband_device device;

    if(device.init_device())
    {
        cerr << "init device failed " << endl;
        return -1;
    }
    
    device.allocate_buffer_using_big_page();
    device.run_test();
    device.free_buffer_using_big_page();
    device.output_big_page_test();

    device.allocate_buffer_using_small_page();
    device.run_test();
    device.free_buffer_using_small_page();
    device.output_small_page_test();

    return 0;
}
