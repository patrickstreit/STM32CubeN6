#ifndef DCMIPP_APP_H
#define DCMIPP_APP_H

#include <stdint.h>
#include "stm32n6570_discovery_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

int dcmipp_is_semiplanar(void);
void dcmipp_get_address(void *luma_address,
                 DCMIPP_FullPlanarDstAddressTypeDef  *planar_address,
                 DCMIPP_SemiPlanarDstAddressTypeDef *semi_planar_address);
int dcmipp_check_config(void);
int dcmipp_config(void *luma_address);
void  dcmipp_set_memory_address(void * next_dcmipp_address);
uint32_t dcmipp_set_csi_phy_bitrate(uint32_t mbps);
HAL_StatusTypeDef  dcmipp_downsize(DCMIPP_HandleTypeDef *hdcmipp,int32_t pipe, int32_t camWidth,int32_t camHeight,int32_t captureWidth,int32_t captureHeight);


#ifdef __cplusplus
}
#endif

#endif // DCMIPP_APP_H