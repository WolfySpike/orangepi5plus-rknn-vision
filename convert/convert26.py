from rknn.api import RKNN

if __name__ == '__main__':
    # 你的 ONNX 模型路径
    ONNX_MODEL = 'sjz-v26s-nms-320.onnx'
    # 导出的 RKNN 模型名称
    RKNN_MODEL = 'sjz-v26s-nms-320.rknn'
    # 量化校准数据集路径（需要您自己新建这个 txt 文件）
    DATASET = 'dataset.txt' 

    # 1. 实例化 RKNN 对象
    rknn = RKNN(verbose=True)

    # 2. 配置 RKNN 参数 (针对 RK3588)
    # 【注意】这里不再传 dataset，只传均值、方差和目标平台
    rknn.config(
        mean_values=[[0, 0, 0]], 
        std_values=[[255, 255, 255]], 
        target_platform='rk3588'
    )

    # 3. 加载 ONNX 模型
    print('--> 正在加载 ONNX 模型...')
    ret = rknn.load_onnx(model=ONNX_MODEL)
    if ret != 0:
        print('加载 ONNX 模型失败！')
        exit(ret)

    # 4. 构建 RKNN 模型
    print('--> 正在构建 RKNN 模型...')
    # 【修正点核心】：do_quantization 设为 True，并且把 dataset 参数放在这里<a href="https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQHXnIpg3pkd1CSYSHnXv3GpMzTW0UNLcdjG7EYnLFc7qCStgwtyCSzukhIDn4yWKfzTCftUW3tNmAoqy62YqKABC2MCd-5ZydA4Eb1akkrSH__lfCPuySSNCmfhrvYpf95uBtdtHm-FM9uchY0w0sG7vsWcXJj9dR1RCjW5Y-Nu55MHJjgeIyo7HE3VU_zf_PP8KfRlFcyHjMfBvbk=" target="_blank" rel="noopener noreferrer" class="citation-ref" title="Source: clehaxze.tw">[1]</a><a href="https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQEM5s2WvynWO-IVtC6TuGCf_UGFZd3FMdM4qo-y3_BlMEgQI-YEMn-5E1OwyYbYa-4tUIB92T7BTEjZE5T7bkHiozcppKQ7M2J7nUcmZBImAXWd2Uf_SG6ZzoW9YjaVRXLJvCqOHwgzLLqs0ZU0TaEnXvOiThvzlUf8FSsbxESfoyD25kVa5xqY9OGNHBjMp6XFyazTYNmN5MmsWhcV9zetRQ==" target="_blank" rel="noopener noreferrer" class="citation-ref" title="Source: dev.to">[2]</a><a href="https://vertexaisearch.cloud.google.com/grounding-api-redirect/AUZIYQFf-ZnUyEuL6t9nLdXSRnRcK6gmbrXENEWAMWYrE0xDcKKcMsWoeMYv5o83vREmC67DFPoEHMhuPqfMecC30IKkh0sRbGw3fo0Kj04g1OF3rraS11qM5slSwi3zSqPGMVQbY9FO_TXGXX4cKS0NeZTr" target="_blank" rel="noopener noreferrer" class="citation-ref" title="Source: eeworld.com.cn">[3]</a>
    ret = rknn.build(do_quantization=True, dataset=DATASET)
    if ret != 0:
        print('构建 RKNN 模型失败！')
        exit(ret)

    # 5. 导出 RKNN 模型
    print('--> 正在导出 RKNN 模型...')
    ret = rknn.export_rknn(RKNN_MODEL)
    if ret != 0:
        print('导出 RKNN 模型失败！')
        exit(ret)

    print('模型转换成功！')
    
    # 6. 释放资源
    rknn.release()
