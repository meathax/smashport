#include "stv/frontend.hpp"
#include <iostream>
#include <stdexcept>

using namespace stv;
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
static void okay(Status s){if(!s)throw std::runtime_error(s.message);}

int main(){try{
    UiPersistent p;p.factory_all();
    bool eligible=true,spent=true;
    // NO_CREDS invalidates the coin checksum but leaves the old bytes alone.
    p.credits=7;p.fractional_units=9;p.bonus_units=11;p.minimum_units=13;
    p.high_score_reset=5;
    CHECK(!p.coin_checksum_valid);
    okay(ui_credit_eligible(p,false,eligible));
    CHECK(!eligible&&p.coin_checksum_valid&&p.credits==0&&p.fractional_units==0);
    CHECK(p.bonus_units==0&&p.minimum_units==0&&p.high_score_reset==5);
    okay(ui_credit_spend(p,false,spent));CHECK(!spent&&p.high_score_reset==5);

    // Source default is one credit for a start or continuation. A custom
    // setting makes the two prices observably distinct.
    p.adjustments[11]=2;p.adjustments[12]=1;p.credits=2;
    okay(ui_credit_eligible(p,false,eligible));CHECK(eligible);
    okay(ui_credit_spend(p,false,spent));CHECK(spent&&p.credits==0&&p.high_score_reset==4);
    okay(ui_credit_spend(p,true,spent));CHECK(!spent&&p.credits==0&&p.high_score_reset==4);
    p.credits=1;okay(ui_credit_spend(p,true,spent));
    CHECK(spent&&p.credits==0&&p.high_score_reset==3);

    // CR_STRTP/CR_CONTP permit free play before coin validation. P_START and
    // P_CONT still run CRED_P, clearing invalid coinage before decrementing HSR.
    p.adjustments[18]=1;p.coin_checksum_valid=false;p.credits=7;
    p.fractional_units=9;p.bonus_units=11;p.minimum_units=13;
    okay(ui_credit_eligible(p,false,eligible));
    CHECK(eligible&&!p.coin_checksum_valid&&p.credits==7);
    okay(ui_credit_spend(p,false,spent));
    CHECK(spent&&p.coin_checksum_valid&&p.credits==0&&p.fractional_units==0);
    CHECK(p.bonus_units==0&&p.minimum_units==0&&p.high_score_reset==2);
    p.credits=3;okay(ui_credit_spend(p,true,spent));
    CHECK(spent&&p.credits==2&&p.high_score_reset==1);
    okay(ui_credit_spend(p,true,spent));
    CHECK(spent&&p.credits==1&&p.high_score_reset==0);
    okay(ui_credit_spend(p,true,spent));
    CHECK(spent&&p.credits==0&&p.high_score_reset==0);

    // A persisted native state can be spent after reload. Out-of-range coin
    // bytes stop at a named boundary rather than wrapping to a valid credit.
    u8 bytes[ui_persistent_size];okay(encode_ui_persistent(p,{bytes,sizeof bytes}));
    UiPersistent restored;okay(decode_ui_persistent({bytes,sizeof bytes},restored));
    CHECK(restored.credits==p.credits&&restored.high_score_reset==p.high_score_reset);
    restored.adjustments[18]=0;restored.credits=256;
    CHECK(!ui_credit_eligible(restored,false,eligible));CHECK(!eligible);
    restored.adjustments[18]=1;
    okay(ui_credit_eligible(restored,false,eligible));CHECK(eligible);
    CHECK(!ui_credit_spend(restored,false,spent));CHECK(!spent&&restored.credits==256);
    // SERVICE is independent of ordinary coin pricing. It audits at max,
    // clears fractional units at max, and ignores presses in diagnostics.
    UiPersistent service;service.factory_all();service.adjustments[17]=5;
    service.credits=4;service.fractional_units=2;service.bonus_units=3;
    service.minimum_units=4;service.game_audits[5]=0xffffffffu;
    bool credited=true;
    okay(ui_service_credit(service,true,credited));
    CHECK(!credited&&!service.coin_checksum_valid&&service.credits==4);
    okay(ui_service_credit(service,false,credited));
    CHECK(credited&&service.coin_checksum_valid&&service.credits==1);
    CHECK(service.game_audits[5]==0&&service.fractional_units==0);
    service.credits=4;service.fractional_units=2;service.bonus_units=3;service.minimum_units=4;
    okay(ui_service_credit(service,false,credited));
    CHECK(credited&&service.credits==5&&service.game_audits[5]==1);
    CHECK(service.fractional_units==0&&service.bonus_units==0&&service.minimum_units==0);
    okay(ui_service_credit(service,false,credited));
    CHECK(credited&&service.credits==5&&service.game_audits[5]==2);
    service.credits=256;CHECK(!ui_service_credit(service,false,credited));
    CHECK(!credited&&service.game_audits[5]==2);
    // VALID_COIN calls ADJUST_CREDITS only after a valid timed pulse. The
    // source's word stores, minimum gate, bonus restart and paid audit differ.
    UiPersistent coin;coin.factory_all();coin.coin_checksum_valid=true;
    coin.adjustments[6]=2;coin.adjustments[7]=3;coin.adjustments[8]=0;coin.adjustments[17]=5;
    u32 paid=99;
    okay(ui_adjust_coin_units(coin,1,paid));
    CHECK(paid==0&&coin.credits==0&&coin.fractional_units==1&&coin.bonus_units==1);
    okay(ui_adjust_coin_units(coin,1,paid));
    CHECK(paid==1&&coin.credits==1&&coin.fractional_units==0&&coin.bonus_units==2&&coin.game_audits[13]==1);
    okay(ui_adjust_coin_units(coin,1,paid));
    CHECK(paid==1&&coin.credits==2&&coin.game_audits[13]==2);
    CHECK(coin.fractional_units==0&&coin.bonus_units==0&&coin.minimum_units==0);
    coin.adjustments[8]=2;
    okay(ui_adjust_coin_units(coin,1,paid));
    CHECK(paid==0&&coin.fractional_units==1&&coin.bonus_units==1&&coin.minimum_units==1);
    okay(ui_adjust_coin_units(coin,1,paid));
    CHECK(paid==1&&coin.credits==3&&coin.fractional_units==0&&coin.bonus_units==2&&coin.minimum_units==0);
    coin.credits=5;coin.fractional_units=1;coin.bonus_units=2;
    okay(ui_adjust_coin_units(coin,1,paid));
    CHECK(paid==0&&coin.credits==5&&coin.fractional_units==1&&coin.bonus_units==2);
    coin.credits=0;coin.adjustments[8]=0;coin.adjustments[7]=0;
    coin.fractional_units=0xffff;coin.bonus_units=0;
    okay(ui_adjust_coin_units(coin,99,paid));
    CHECK(paid==49&&coin.credits==5&&coin.game_audits[13]==52);
    CHECK(coin.fractional_units==0&&coin.bonus_units==0);
    coin.fractional_units=0x10000;CHECK(!ui_adjust_coin_units(coin,1,paid));
    CHECK(paid==0&&coin.fractional_units==0x10000);
    std::cout<<"PASS isolated source credit check/spend, service and coin units, deferred recovery and reload\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
