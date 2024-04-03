#include"DataAcquisition.h"

using namespace std;

int main(int argc,char const *argv[]){
    int retVal=0;
    DataAcquisition dataAcquisition;
    retVal = dataAcquisition.run();
    return retVal;
}