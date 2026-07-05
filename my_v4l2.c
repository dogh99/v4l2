#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/videodev2.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include <media/v4l2-device.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-fh.h>
#include <linux/string.h>

#define V4L2_PIX_FMT_MYRAW v4l2_fourcc('M', 'Y', 'R', 'W')  //没有实际意义,只是格式名称的编码,4位

struct my_v4l2_dev {
    struct device* dev;
    struct v4l2_device v4l2_dev; //总的管理对象
    struct video_device vdev;   //video_device里有一个指针指向v4l2_device]
    struct mutex lock;

    u32 width;
	u32 height;
	u32 pixelformat;
};

struct imx415 {
    struct v4l2_subdev sd;        // 摄像头 sensor 子设备
    struct i2c_client *client;
};


int my_v4l2_open (struct file* file)
{
    // struct my_v4l2_dev* my_dev = video_drvdata(file);
    // return v4l2_fh_open(file);
    return 0;
}

int my_v4l2_release (struct file* file)
{
    return 0;
}



static struct v4l2_file_operations my_v4l2_ops = {
    .owner = THIS_MODULE,
    .open = my_v4l2_open,
    .release = my_v4l2_release,
    .unlocked_ioctl = video_ioctl2,
}; 

int my_querycap(struct file* file, void* fh,       //能力查询
			       struct v4l2_capability* cap)   //cap是用户态传入的参数,用来接收结果
{
    struct my_v4l2_dev* dev = video_drvdata(file);
    strscpy(cap->driver, "my_v4l2", sizeof(cap->driver));   //安全字符串拷贝函数
    strscpy(cap->card, "my_v4l2 camera", sizeof(cap->card));   //安全字符串拷贝函数
    snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s", dev_name(dev->dev));
    cap->device_caps = V4L2_CAP_VIDEO_CAPTURE;
    cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
    return 0;
}

int my_enum_fmt(struct file *file, void *fh,      //支持的格式
				       struct v4l2_fmtdesc *f)    //这里的f也是用户传进来的ioctl参数
{

    //对于f->description,v4l2会对其进行标准化,所以填进去没什么意义,但是pixelformat需要设置
    switch (f->index) {
    case 0:
        f->pixelformat = V4L2_PIX_FMT_YUYV;
        strscpy(f->description, "YUYV 4:2:2 megumi edition", sizeof(f->description));
        return 0;

    case 1:
        f->pixelformat = V4L2_PIX_FMT_NV12;
        strscpy(f->description, "NV12 megumi edition", sizeof(f->description));
        return 0;

    case 2:
        f->pixelformat = V4L2_PIX_FMT_RGB565;
        strscpy(f->description, "RGB565 megumi edition", sizeof(f->description));
        return 0;

    case 3:   //我的自定义格式,v4l2 core 遇到自定义格式时会保留description
        f->pixelformat = V4L2_PIX_FMT_MYRAW;
        strscpy(f->description, "My private RAW format megumi edition!!!", sizeof(f->description));
        return 0;
    default:
        return -EINVAL;
    }
}


int my_g_fmt(struct file *file, void *fh, struct v4l2_format *f)  //告诉用户态当前设备正在使用什么图像格式, f也是用户态传入的参数
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


int my_try_fmt(struct file *file, void *fh, struct v4l2_format *f) //检查用户想设置的格式是否支持,并把不合理的地方修正为驱动能够接收的值, 但不保存到设备状态里面
{
    struct v4l2_pix_format* pix = &f->fmt.pix;

    if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE){
        return -EINVAL;
    }
    if (pix->pixelformat != V4L2_PIX_FMT_YUYV)
        pix->pixelformat = V4L2_PIX_FMT_YUYV;

    pix->width = clamp(pix->width, 320U, 1920U); //clamp宏,将数值限制到后面两个参数的范围内
    pix->height = clamp(pix->height, 240U, 1080U);

    pix->field = V4L2_FIELD_NONE;
    pix->bytesperline = pix->width * 2;
    pix->sizeimage = pix->bytesperline * pix->height;
    pix->colorspace = V4L2_COLORSPACE_SRGB;

    return 0;
}

int my_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
    struct my_v4l2_dev* dev = video_drvdata(file);
    struct v4l2_pix_format* pix = &f->fmt.pix;
    int ret;

    ret = my_try_fmt(file, fh, f); //对传入的格式进行检查
    if (ret){
        return ret;
    }
    
    dev->width = pix->width;
    dev->height = pix->height;
    dev->pixelformat = pix->pixelformat;

    return 0;
}


static struct v4l2_ioctl_ops my_v4l2_ioctl_ops = {
    .vidioc_querycap = my_querycap,    //查询设备能力
    .vidioc_enum_fmt_vid_cap = my_enum_fmt, //枚举支持的图像格式
	.vidioc_g_fmt_vid_cap = my_g_fmt,    //获取当前的格式
	.vidioc_try_fmt_vid_cap = my_try_fmt, // 尝试设置格式但是不保存
	.vidioc_s_fmt_vid_cap = my_s_fmt,   // 设置格式
};


static int my_v4l2_probe(struct platform_device* pdev)
{
    int ret;
    struct my_v4l2_dev* my_dev;
    my_dev = devm_kzalloc(&pdev->dev, sizeof(struct my_v4l2_dev), GFP_KERNEL);
    if (!my_dev)
    {
        return -ENOMEM;
    }
    my_dev->dev = &pdev->dev;
    mutex_init(&my_dev->lock);
    my_dev->width = 640;
    my_dev->height = 480;
    my_dev->pixelformat = V4L2_PIX_FMT_YUYV;

    ret = v4l2_device_register(&pdev->dev, &my_dev->v4l2_dev);  //注册v4l2_device
    if (ret)
    {
        return ret;
    }
    strscpy(my_dev->vdev.name, "my-v4l2", sizeof(my_dev->vdev.name));
    my_dev->vdev.v4l2_dev = &my_dev->v4l2_dev;  //关联video_device与v4l2_dev
    my_dev->vdev.fops = &my_v4l2_ops; //分配操作符
    my_dev->vdev.release = video_device_release_empty; //这是一个空函数,适合struct video_device 嵌入到私有结构体的形式 vdev.release是释放回调
    my_dev->vdev.ioctl_ops = &my_v4l2_ioctl_ops;
    my_dev->vdev.lock = &my_dev->lock;
    my_dev->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE;//申明能力

    video_set_drvdata(&my_dev->vdev, my_dev); //设置私密数据
    platform_set_drvdata(pdev, my_dev);

    ret = video_register_device(&my_dev->vdev, VFL_TYPE_VIDEO, -1); //创建节点/dev/Videox  第三个参数为自动找空闲编号 

    if (ret) {
		v4l2_device_unregister(&my_dev->v4l2_dev);
		return ret;
	}
    dev_info(&pdev->dev, "registered %s\n", video_device_node_name(&my_dev->vdev));
    printk("probe okay desuwa!!\n");


    return 0;        
}

static int my_v4l2_remove(struct platform_device* pdev)
{
    struct my_v4l2_dev* my_dev = platform_get_drvdata(pdev);
    video_unregister_device(&my_dev->vdev);
    v4l2_device_unregister(&my_dev->v4l2_dev);
    printk("remove okay desuwa!!\n");
    return 0;
}

static const struct of_device_id my_v4l2_of_match[] = {
    { .compatible = "megumi,my-v4l2"},
    {}
};

static struct platform_driver my_v4l2_driver ={
    .probe = my_v4l2_probe,
    .remove = my_v4l2_remove,
    .driver = {
        .name = "megumi,my-v4l2",
        .of_match_table = my_v4l2_of_match,
    },
};



static int my_v4l2_init(void)
{
    return platform_driver_register(&my_v4l2_driver); //把驱动注册到总线去匹配设备
    return 0;
}



static void my_v4l2_exit(void)
{
    platform_driver_unregister(&my_v4l2_driver);
}

module_init(my_v4l2_init);
module_exit(my_v4l2_exit);
MODULE_LICENSE("GPL");



