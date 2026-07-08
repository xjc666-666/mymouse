#include "PAW3395.h"

/*********************************************************************
 * 1. 软件模拟 SPI (严格遵循原相 Mode 3 时序)
 * 空闲高电平，下降沿发送，上升沿读取。绝对防死机！
 *********************************************************************/
static uint8_t PAW_SPI_TxRx(uint8_t data)
{
    uint8_t rx_data = 0;
    
    for(uint8_t i = 0; i < 8; i++)
    {
        // 1. 时钟拉低 (下降沿)，准备推入数据
        GPIOA_ResetBits(PAW_SCK_PIN); 
        
        if(data & 0x80) GPIOA_SetBits(PAW_MOSI_PIN);
        else            GPIOA_ResetBits(PAW_MOSI_PIN);
        data <<= 1;
        
        DelayUs(1); // 保证数据建立时间
        
        // 2. 时钟拉高 (上升沿)，读取传感器传回的数据
        GPIOA_SetBits(PAW_SCK_PIN);
        
        rx_data <<= 1;
        // 如果读到高电平，写1
        if(GPIOA_ReadPortPin(PAW_MISO_PIN)) {
            rx_data |= 0x01;
        }
        
        DelayUs(1); // 保证数据保持时间
    }
    
    return rx_data;
}

/*********************************************************************
 * 2. 底层读写寄存器
 *********************************************************************/
static void writr_register(uint8_t reg, uint8_t data)
{
    PAW_CS_LOW();
    DelayUs(1);
    PAW_SPI_TxRx(reg | 0x80); 
    PAW_SPI_TxRx(data);
    PAW_CS_HIGH();
    DelayUs(5); 
}

static uint8_t read_register(uint8_t reg)
{
    uint8_t res;
    PAW_CS_LOW();
    DelayUs(1);
    PAW_SPI_TxRx(reg & 0x7F); 
    DelayUs(4); // 发送地址后必须等待
    
    res = PAW_SPI_TxRx(0x00); // 喂入纯净时钟，抽取真数据
    
    PAW_CS_HIGH();
    DelayUs(2); 
    return res;
}

uint8_t PAW3395_GetID(void) {
    return read_register(0x00);
}

/*********************************************************************
 * 3. 绝密上电初始化序列 (官方 100 多行)
 *********************************************************************/
static void Power_Up_Initializaton_Register_Setting(void)
{
    uint8_t read_tmp;
    uint8_t i;
    
    writr_register(0x7F ,0x07);  writr_register(0x40 ,0x41);
    writr_register(0x7F ,0x00);  writr_register(0x40 ,0x80);
    writr_register(0x7F ,0x0E);  writr_register(0x55 ,0x0D);
    writr_register(0x56 ,0x1B);  writr_register(0x57 ,0xE8);
    writr_register(0x58 ,0xD5);  writr_register(0x7F ,0x14);
    writr_register(0x42 ,0xBC);  writr_register(0x43 ,0x74);
    writr_register(0x4B ,0x20);  writr_register(0x4D ,0x00);
    writr_register(0x53 ,0x0E);  writr_register(0x7F ,0x05);
    writr_register(0x44 ,0x04);  writr_register(0x4D ,0x06);
    writr_register(0x51 ,0x40);  writr_register(0x53 ,0x40);
    writr_register(0x55 ,0xCA);  writr_register(0x5A ,0xE8);
    writr_register(0x5B ,0xEA);  writr_register(0x61 ,0x31);
    writr_register(0x62 ,0x64);  writr_register(0x6D ,0xB8);
    writr_register(0x6E ,0x0F);  writr_register(0x70 ,0x02);
    writr_register(0x4A ,0x2A);  writr_register(0x60 ,0x26);
    writr_register(0x7F ,0x06);  writr_register(0x6D ,0x70);
    writr_register(0x6E ,0x60);  writr_register(0x6F ,0x04);
    writr_register(0x53 ,0x02);  writr_register(0x55 ,0x11);
    writr_register(0x7A ,0x01);  writr_register(0x7D ,0x51);
    writr_register(0x7F ,0x07);  writr_register(0x41 ,0x10);
    writr_register(0x42 ,0x32);  writr_register(0x43 ,0x00);
    writr_register(0x7F ,0x08);  writr_register(0x71 ,0x4F);
    writr_register(0x7F ,0x09);  writr_register(0x62 ,0x1F);
    writr_register(0x63 ,0x1F);  writr_register(0x65 ,0x03);
    writr_register(0x66 ,0x03);  writr_register(0x67 ,0x1F);
    writr_register(0x68 ,0x1F);  writr_register(0x69 ,0x03);
    writr_register(0x6A ,0x03);  writr_register(0x6C ,0x1F);
    writr_register(0x6D ,0x1F);  writr_register(0x51 ,0x04);
    writr_register(0x53 ,0x20);  writr_register(0x54 ,0x20);
    writr_register(0x71 ,0x0C);  writr_register(0x72 ,0x07);
    writr_register(0x73 ,0x07);  writr_register(0x7F ,0x0A);
    writr_register(0x4A ,0x14);  writr_register(0x4C ,0x14);
    writr_register(0x55 ,0x19);  writr_register(0x7F ,0x14);
    writr_register(0x4B ,0x30);  writr_register(0x4C ,0x03);
    writr_register(0x61 ,0x0B);  writr_register(0x62 ,0x0A);
    writr_register(0x63 ,0x02);  writr_register(0x7F ,0x15);
    writr_register(0x4C ,0x02);  writr_register(0x56 ,0x02);
    writr_register(0x41 ,0x91);  writr_register(0x4D ,0x0A);
    writr_register(0x7F ,0x0C);  writr_register(0x4A ,0x10);
    writr_register(0x4B ,0x0C);  writr_register(0x4C ,0x40);
    writr_register(0x41 ,0x25);  writr_register(0x55 ,0x18);
    writr_register(0x56 ,0x14);  writr_register(0x49 ,0x0A);
    writr_register(0x42 ,0x00);  writr_register(0x43 ,0x2D);
    writr_register(0x44 ,0x0C);  writr_register(0x54 ,0x1A);
    writr_register(0x5A ,0x0D);  writr_register(0x5F ,0x1E);
    writr_register(0x5B ,0x05);  writr_register(0x5E ,0x0F);
    writr_register(0x7F ,0x0D);  writr_register(0x48 ,0xDD);
    writr_register(0x4F ,0x03);  writr_register(0x52 ,0x49);
    writr_register(0x51 ,0x00);  writr_register(0x54 ,0x5B);
    writr_register(0x53 ,0x00);  writr_register(0x56 ,0x64);
    writr_register(0x55 ,0x00);  writr_register(0x58 ,0xA5);
    writr_register(0x57 ,0x02);  writr_register(0x5A ,0x29);
    writr_register(0x5B ,0x47);  writr_register(0x5C ,0x81);
    writr_register(0x5D ,0x40);  writr_register(0x71 ,0xDC);
    writr_register(0x70 ,0x07);  writr_register(0x73 ,0x00);
    writr_register(0x72 ,0x08);  writr_register(0x75 ,0xDC);
    writr_register(0x74 ,0x07);  writr_register(0x77 ,0x00);
    writr_register(0x76 ,0x08);  writr_register(0x7F ,0x10);
    writr_register(0x4C ,0xD0);  writr_register(0x7F ,0x00);
    writr_register(0x4F ,0x63);  writr_register(0x4E ,0x00);
    writr_register(0x52 ,0x63);  writr_register(0x51 ,0x00);
    writr_register(0x54 ,0x54);  writr_register(0x5A ,0x10);
    writr_register(0x77 ,0x4F);  writr_register(0x47 ,0x01);
    writr_register(0x5B ,0x40);  writr_register(0x64 ,0x60);
    writr_register(0x65 ,0x06);  writr_register(0x66 ,0x13);
    writr_register(0x67 ,0x0F);  writr_register(0x78 ,0x01);
    writr_register(0x79 ,0x9C);  writr_register(0x40 ,0x00);
    writr_register(0x55 ,0x02);  writr_register(0x23 ,0x70);
    writr_register(0x22 ,0x01);

    DelayMs(1);
    for(i = 0; i < 60; i++) {
        read_tmp = read_register(0x6C);
        if(read_tmp == 0x80) break;
        DelayMs(1);
    }
    if(i == 60) {
        writr_register(0x7F, 0x14);
        writr_register(0x6C, 0x00);
        writr_register(0x7F, 0x00);
    }
    writr_register(0x22, 0x00);
    writr_register(0x55, 0x00);
    writr_register(0x7F, 0x07);
    writr_register(0x40, 0x40);
    writr_register(0x7F, 0x00);
}

/*********************************************************************
 * 4. 传感器初始化 (强制接管为纯净 GPIO)
 *********************************************************************/
void PAW3395_Init(void)
{
    uint8_t reg_it;

    // 所有 SPI 引脚定义为纯 GPIO！不调用任何外设 SPI 初始化！
    GPIOA_ModeCfg(PAW_CS_PIN | PAW_SCK_PIN | PAW_MOSI_PIN | PAW_RESET_PIN, GPIO_ModeOut_PP_5mA);
    GPIOA_ModeCfg(PAW_MISO_PIN | PAW_MOTION_PIN, GPIO_ModeIN_PU); 
    
    // 初始化空闲状态 (严格遵守原相要求)
    PAW_CS_HIGH();
    GPIOA_SetBits(PAW_SCK_PIN);   // SPI Mode 3: 时钟平时必须是高电平
    GPIOA_ResetBits(PAW_MOSI_PIN);

    // 硬件复位打浪时序
    DelayMs(50);
    GPIOA_ResetBits(PAW_RESET_PIN);
    DelayMs(5);
    GPIOA_SetBits(PAW_RESET_PIN);
    DelayMs(50);
    
    PAW_CS_LOW();  DelayUs(1);
    PAW_CS_HIGH(); DelayUs(1);
    PAW_CS_LOW();  DelayUs(1);
        
    writr_register(0x3A, 0x5A); 
    DelayMs(5);
    
    Power_Up_Initializaton_Register_Setting();
    
    PAW_CS_HIGH();
    DelayUs(1);
    
    for(reg_it = 0x02; reg_it <= 0x06; reg_it++) {    
        read_register(reg_it);
        DelayUs(2);   
    }
    PAW_CS_HIGH();
    DelayUs(1);
}

/*********************************************************************
 * 5. 极速读取运动包 (Motion Burst)
 *********************************************************************/
void PAW3395_ReadMotion(PAW3395_Data_t *data)
{
    uint8_t buffer[12];
    
    PAW_CS_LOW();
    DelayUs(1);
    
    PAW_SPI_TxRx(0x16); // 突发读地址
    DelayUs(4); 
    
    for(uint8_t i = 0; i < 12; i++) {
        buffer[i] = PAW_SPI_TxRx(0x00); // 连续提取 12 字节
    }
    
    PAW_CS_HIGH();
    DelayUs(1);

    data->rawMotion = buffer[0]; 
    data->squal = buffer[6]; 
    
    data->isMotion = (buffer[0] & 0x80) ? 1 : 0;
    
    if (data->isMotion) {
        data->deltaX = (int16_t)((buffer[3] << 8) | buffer[2]);
        data->deltaY = (int16_t)((buffer[5] << 8) | buffer[4]);
    } else {
        data->deltaX = 0;
        data->deltaY = 0;
    }
}

/*********************************************************************
 * 6. 配置 DPI
 *********************************************************************/
void PAW3395_SetDPI(uint16_t cpi_val)
{
    uint8_t temp;
    PAW_CS_LOW();
    DelayUs(1);
    
    writr_register(0x5C, 0x00); 
    
    temp = (uint8_t)(((cpi_val/50) << 8) >> 8);
    writr_register(0x48, temp); 
    temp = (uint8_t)((cpi_val/50) >> 8);
    writr_register(0x49, temp); 
    
    writr_register(0x47, 0x01); 
    
    PAW_CS_HIGH();
    DelayUs(1);
}