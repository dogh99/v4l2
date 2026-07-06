#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/videodev2.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#define V4L2_PIX_FMT_MYRAW \
    v4l2_fourcc('M', 'Y', 'R', 'W')  // 没有实际意义,只是格式名称的编码,4位

struct my_v4l2_dev {
    struct device* dev;   //用来存储通用设备信息,如果你想,也可用存platform device等等
    struct v4l2_device v4l2_dev;  // 总的管理对象
    struct video_device vdev;  // video_device里有一个指针指向v4l2_device]
    struct mutex lock;
    struct vb2_queue queue;
    struct list_head buf_list;
    spinlock_t qlock;
    bool streaming;
    unsigned int sequence;


    u32 width;
    u32 height;
    u32 pixelformat;
};


struct my_v4l2_buffer{
    struct vb2_v4l2_buffer vb;  //一个buffer通常对应一帧
    struct list_head list;
};

struct imx415 {
    struct v4l2_subdev sd;  // 摄像头 sensor 子设备
    struct i2c_client* client;
};

int my_v4l2_open(struct file* file) {
    v4l2_fh_open(
        file);  // fh是file
                // handle,初始化一个v4l2_fh并放入file的privatedata中,对应这一次open对应的独立上下文
    // 在struct file的privatedata里会存放这个fh,描述的是这一次打开设备的 V4L2
    // 会话状态 不同的打开者的数据之间相互独立
    return 0;
}

int my_v4l2_release(struct file* file) {
    v4l2_fh_release(file);
    return 0;
}

static struct v4l2_file_operations my_v4l2_ops = {
    .owner = THIS_MODULE,
    .open = my_v4l2_open,
    .release = my_v4l2_release,
    .unlocked_ioctl = video_ioctl2,
    .read = vb2_fop_read,  //将内核空间的数据拷贝到用户空间,与mmap是相反的,用处不大
    .poll = vb2_fop_poll,
    .mmap = vb2_fop_mmap,  //映射虚拟内存空间

};

int my_querycap(
    struct file* file, void* fh,  // 能力查询
    struct v4l2_capability* cap)  // cap是用户态传入的参数,用来接收结果
{
    struct my_v4l2_dev* dev = video_drvdata(file);
    strscpy(cap->driver, "my_v4l2", sizeof(cap->driver));  // 安全字符串拷贝函数
    strscpy(cap->card, "my_v4l2 camera",
            sizeof(cap->card));  // 安全字符串拷贝函数
    snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
             dev_name(dev->dev));
    cap->device_caps = V4L2_CAP_VIDEO_CAPTURE;
    cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
    return 0;
}

int my_enum_fmt(struct file* file, void* fh,  // 支持的格式
                struct v4l2_fmtdesc* f)  // 这里的f也是用户传进来的ioctl参数
{
    // 对于f->description,v4l2会对其进行标准化,所以填进去没什么意义,但是pixelformat需要设置
    switch (f->index) {
        case 0:
            f->pixelformat = V4L2_PIX_FMT_YUYV;
            strscpy(f->description, "YUYV 4:2:2 megumi edition",
                    sizeof(f->description));
            return 0;
        default:
            return -EINVAL;
    }
}

int my_g_fmt(struct file* file, void* fh,
             struct v4l2_format* f)  // 告诉用户态当前设备正在使用什么图像格式,
                                     // f也是用户态传入的参数
{
    struct my_v4l2_dev* dev = video_drvdata(file);
    struct v4l2_pix_format* pix = &f->fmt.pix;

    f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    pix->width = dev->width;
    pix->height = dev->height;
    pix->pixelformat = dev->pixelformat;
    pix->field = V4L2_FIELD_NONE;

    pix->bytesperline = dev->width * 2;
    pix->sizeimage = pix->bytesperline * dev->height;
    pix->colorspace = V4L2_COLORSPACE_SRGB;

    return 0;
}

int my_try_fmt(struct file* file, void* fh,
               struct v4l2_format* f)  // 检查用户想设置的格式是否支持,并把不合理的地方修正为驱动能够接收的值,
                                       // 但不保存到设备状态里面
{
    struct v4l2_pix_format* pix = &f->fmt.pix;

    if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        return -EINVAL;
    }
    if (pix->pixelformat != V4L2_PIX_FMT_YUYV)
        pix->pixelformat = V4L2_PIX_FMT_YUYV;

    pix->width = clamp(pix->width, 320U,
                       1920U);  // clamp宏,将数值限制到后面两个参数的范围内
    pix->height = clamp(pix->height, 240U, 1080U);

    pix->field = V4L2_FIELD_NONE;
    pix->bytesperline = pix->width * 2;
    pix->sizeimage = pix->bytesperline * pix->height;
    pix->colorspace = V4L2_COLORSPACE_SRGB;

    return 0;
}

int my_s_fmt(struct file* file, void* fh, struct v4l2_format* f) {
    struct my_v4l2_dev* dev = video_drvdata(file);
    struct v4l2_pix_format* pix = &f->fmt.pix;
    int ret;

    ret = my_try_fmt(file, fh, f);  // 对传入的格式进行检查
    if (ret) {
        return ret;
    }

    dev->width = pix->width;
    dev->height = pix->height;
    dev->pixelformat = pix->pixelformat;

    return 0;
}

int my_enum_input(struct file* file, void* fh, struct v4l2_input* inp) { //枚举输入源
    if (inp->index != 0) {
        return -EINVAL;
    }
    strscpy(inp->name, "Megumi Camera", sizeof(inp->name));
    inp->type = V4L2_INPUT_TYPE_CAMERA;
    inp->status = 0;

    return 0;
}

int my_g_input(struct file* file, void* fh, unsigned int* i) { //当前使用的哪一个输入源
    *i = 0;
    return 0;
}

int my_s_input(struct file* file, void* fh, unsigned int i) {   //切换输入源
    if (i != 0) {
        return -EINVAL;
    }


    return 0;
}


static int my_vb2_init(struct my_v4l2_dev* dev){
    int ret;
    INIT_LIST_HEAD(&dev->buf_list);
    spin_lock_init(&dev->qlock);

    dev->queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    dev->queue.io_modes = VB2_MMAP;
    dev->queue.drv_priv = dev; //存放私有数据
    dev->queue.buf_struct_size = sizeof(struct my_v4l2_buffer);
    dev->queue.ops = &my_vb2_ops;

    dev->queue.mem_ops = &vb2_vmalloc_memops; //这条视频队列的 buffer 内存，使用 vmalloc 方式来分配、映射和释放。
    
    dev->queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
    dev->queue.lock = &dev->lock;

    dev->queue.dev = &dev->dev;

    ret = vb2_queue_init(&dev->queue);

    return ret;
}



static int my_queue_setup(struct vb2_queue *q, //初始化的队列
                        unsigned int* num_buffers, //分配的buffer数量,来自用户态
                        unsigned int* num_planes,  //每个buffer有多少个plane,来自用户态
                        unsigned int* sizes,       //每个plane多大,来自用户态
                        struct device** alloc_devs){

    struct my_v4l2_dev* dev = vb2_get_drv_priv(q);
    unsigned int size = dev->width * dev->height * 2; //每一帧的大小


    if (*num_planes)
    {
        return sizes[0] < size ? -EINVAL : 0;
    }
    
    *num_planes = 1;
    sizes[0] = size;

    
    return 0;

}

int my_buf_prepare(struct vb2_buffer *vb)
{
    struct my_v4l2_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
    unsigned int size = dev->width * dev->height * 2;

   if (vb2_plane_size(vb, 0) < size)
        return -EINVAL;

    vb2_set_plane_payload(vb, 0, size);
    return 0;
}

void my_buf_queue(struct vb2_buffer *vb)
//当用户态调用 VIDIOC_QBUF 把一个空 buffer 交给驱动时，vb2 会调用 my_buf_queue()；该函数把这个 buffer 放进驱动自己的 dev->buf_list 等待队列，供后续 DMA/硬件使用。
{
    struct my_v4l2_dev *dev = vb2_get_drv_priv(vb->vb2_queue);

    struct my_v4l2_buffer* buf = 
    container_of(to_vb2_v4l2_buffer(vb), struct my_v4l2_buffer, vb);
    //获取自定义buf对象

    unsigned long flags;
    spin_lock_irqsave(&dev->qlock, flags); //获取自旋锁,加锁保护
    list_add_tail(&buf->list, &dev->buf_list); //把当前buffer加入驱动私有队列尾部
    spin_unlock_irqrestore(&dev->qlock, flags);
}

static int my_start_streaming(struct vb2_queue *vq, unsigned int count)
{
    struct my_v4l2_dev *dev = vb2_get_drv_priv(vq);

    dev->streaming = true;
    dev->sequence = 0;

    return 0;
}

static void my_stop_streaming(struct vb2_queue *vq)
//TODO
{
    struct my_v4l2_dev *dev = vb2_get_drv_priv(vq);
    struct my_v4l2_buffer *buf, *tmp;
    unsigned long flags;

    dev->streaming = false;

    spin_lock_irqsave(&dev->qlock, flags);
    list_for_each_entry_safe(buf, tmp, &dev->buf_list, list) {
        list_del(&buf->list);
        vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
    }
    spin_unlock_irqrestore(&dev->qlock, flags);
}



static const struct vb2_ops my_vb2_ops = {
    .queue_setup = my_queue_setup,
    .buf_prepare = my_buf_prepare,
    .buf_queue = my_buf_queue,
    .start_streaming = my_start_streaming,
    .stop_streaming = my_stop_streaming,
    .wait_prepare = vb2_ops_wait_prepare,
    .wait_finish = vb2_ops_wait_finish,
};




static struct v4l2_ioctl_ops my_v4l2_ioctl_ops = {
    .vidioc_querycap = my_querycap,          // 查询设备能力
    .vidioc_enum_fmt_vid_cap = my_enum_fmt,  // 枚举支持的图像格式
    .vidioc_g_fmt_vid_cap = my_g_fmt,        // 获取当前的格式
    .vidioc_try_fmt_vid_cap = my_try_fmt,    // 尝试设置格式但是不保存
    .vidioc_s_fmt_vid_cap = my_s_fmt,        // 设置格式
    .vidioc_enum_input = my_enum_input,      // 查看所有输入源
    .vidioc_g_input = my_g_input,            // 查看当前输入源
    .vidioc_s_input = my_s_input,            // 设置输入源

    .vidioc_reqbufs = vb2_ioctl_reqbufs,
};

static int my_v4l2_probe(struct platform_device* pdev) {
    int ret;
    struct my_v4l2_dev* my_dev;
    my_dev = devm_kzalloc(&pdev->dev, sizeof(struct my_v4l2_dev), GFP_KERNEL);
    if (!my_dev) {
        return -ENOMEM;
    }
    my_dev->dev = &pdev->dev;
    mutex_init(&my_dev->lock);
    my_dev->width = 640;
    my_dev->height = 480;
    my_dev->pixelformat = V4L2_PIX_FMT_YUYV;

    ret =
        v4l2_device_register(&pdev->dev, &my_dev->v4l2_dev);  // 注册v4l2_device
    if (ret) {
        return ret;
    }
    strscpy(my_dev->vdev.name, "my-v4l2", sizeof(my_dev->vdev.name));
    my_dev->vdev.v4l2_dev = &my_dev->v4l2_dev;  // 关联video_device与v4l2_dev
    my_dev->vdev.fops = &my_v4l2_ops;           // 分配操作符
    my_dev->vdev.release =
        video_device_release_empty;  // 这是一个空函数,适合struct video_device
                                     // 嵌入到私有结构体的形式
                                     // vdev.release是释放回调
    my_dev->vdev.ioctl_ops = &my_v4l2_ioctl_ops;
    my_dev->vdev.lock = &my_dev->lock;
    my_dev->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE;  // 申明能力

    video_set_drvdata(&my_dev->vdev, my_dev);  // 设置私密数据
    platform_set_drvdata(pdev, my_dev);

    ret = video_register_device(
        &my_dev->vdev, VFL_TYPE_VIDEO,
        -1);  // 创建节点/dev/Videox  第三个参数为自动找空闲编号

    if (ret) {
        v4l2_device_unregister(&my_dev->v4l2_dev);
        return ret;
    }

    ret = my_vb2_init(my_dev);
    if (ret) {
        v4l2_device_unregister(&my_dev->v4l2_dev);
        return ret;
    }
    my_dev->vdev.queue = &my_dev->queue;


    dev_info(&pdev->dev, "registered %s\n",
             video_device_node_name(&my_dev->vdev));
    printk("probe okay desuwa!!\n");

    return 0;
}

static int my_v4l2_remove(struct platform_device* pdev) {
    struct my_v4l2_dev* my_dev = platform_get_drvdata(pdev);
    video_unregister_device(&my_dev->vdev);
    v4l2_device_unregister(&my_dev->v4l2_dev);
    printk("remove okay desuwa!!\n");
    return 0;
}

static const struct of_device_id my_v4l2_of_match[] = {
    {.compatible = "megumi,my-v4l2"}, {}};

static struct platform_driver my_v4l2_driver = {
    .probe = my_v4l2_probe,
    .remove = my_v4l2_remove,
    .driver =
        {
            .name = "megumi,my-v4l2",
            .of_match_table = my_v4l2_of_match,
        },
};

static int my_v4l2_init(void) {
    return platform_driver_register(
        &my_v4l2_driver);  // 把驱动注册到总线去匹配设备
    return 0;
}

static void my_v4l2_exit(void) { platform_driver_unregister(&my_v4l2_driver); }

module_init(my_v4l2_init);
module_exit(my_v4l2_exit);
MODULE_LICENSE("GPL");
