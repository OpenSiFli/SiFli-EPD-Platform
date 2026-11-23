#include "host.h"
#include "decode.inc"
#include "store.inc"
#include "catalog.inc"
static gallery_result_t result;
static void snapshot(gallery_job_t*job){memset(&result,0,sizeof(result));gallery_catalog_page(job,&result);}
static void page(gallery_job_t*job){for(unsigned turn=0;turn<20000;++turn){snapshot(job);if(result.complete||result.error_code||gallery_cancelled(job->serial)||!present)break;(void)gallery_catalog_step();}while(gallery_catalog_step()){}if(!gallery_cancelled(job->serial)&&present&&!result.error_code)snapshot(job);}
static void release(gallery_result_t*r){epd_app_free(r->pixels);r->pixels=NULL;}
static bool render_and_save(const gallery_job_t*j,gallery_decoder_t*d,gallery_result_t*r){bool ok=gallery_cached_render(j,d,r);if(ok){if(!r->references)r->references=1;gallery_cache_schedule(j,r);while(gallery_cache_step()){} }return ok;}
#define gallery_cached_render render_and_save
int main(void){
 CHECK(gallery_sd_path("/sdcard"));CHECK(gallery_sd_path("/sdcard/a/.hidden.PNG"));
 CHECK(!gallery_sd_path("/flash/a.png"));CHECK(!gallery_sd_path("/sdcard2/a.png"));CHECK(!gallery_sd_path("/sdcard/../flash/a.png"));CHECK(!gallery_sd_path("/sdcard//a.png"));
 add("/sdcard",true,0);add("/sdcard/picture",true,0);add("/sdcard/picture/Trip",true,0);add("/sdcard/.hidden",true,0);
 add("/sdcard/picture/Trip/new.JPEG",false,500);add("/sdcard/.hidden/.photo.PNG",false,999);add("/sdcard/root.jpg",false,1);add("/sdcard/notes.txt",false,1);
 add("/sdcard/System Volume Information",true,0);add("/sdcard/.epd",true,0);add("/sdcard/Notes",true,0);
 add("/sdcard/Notes/readme.txt",false,1);add("/sdcard/Notes/unsupported.gif",false,1);add("/sdcard/empty.JPG",true,0);
 for(unsigned i=0;i<1200;++i){char p[512];snprintf(p,512,"/sdcard/picture/%04u.png",i);add(p,false,i);}
 char deep[512]="/sdcard/nest";add(deep,true,0);for(unsigned i=0;i<64;++i){strcat(deep,"/a");add(deep,true,0);}strcat(deep,"/deep.JpG");add(deep,false,1);
 gallery_job_t job={.serial=1,.kind=GALLERY_SCAN,.recursive=true};strcpy(job.path,"/sdcard");
 // Directories alone are not albums; the first actual image creates a cover.
 job.kind=GALLERY_ALBUMS;job.recursive=false;snapshot(&job);CHECK(!result.count&&!result.complete&&scanning&&reads<10);
 unsigned saved_records=records,saved_opens=opens;cancelled=true;snapshot(&job);CHECK(valid&&scanning&&records==saved_records&&opens==saved_opens);cancelled=false;
 storage_lock();CHECK(gallery_catalog_revision()&&gallery_catalog_generation());storage_unlock();
 CHECK(gallery_catalog_step());snapshot(&job);CHECK(!result.complete&&result.total==1&&result.directories==1&&scanning&&reads<100);
 CHECK(!strcmp(result.entries[0].path,"/sdcard")&&!strcmp(result.entries[0].cover,"/sdcard/root.jpg"));
 unsigned root_opens=opens;snapshot(&job);CHECK(opens==root_opens);
 job.kind=GALLERY_SCAN;CHECK(gallery_catalog_step());snapshot(&job);CHECK(result.complete&&result.total==1&&!result.directories&&scanning);
 job.recursive=true;snapshot(&job);CHECK(!result.complete&&scanning);CHECK(gallery_catalog_step());snapshot(&job);CHECK(result.total>0&&!result.complete);
 page(&job);CHECK(!result.error_code&&result.count==9&&result.total==1204&&result.directories==0&&result.more);
 unsigned expected_opens=0;for(unsigned i=0;i<file_count;++i)if(files[i].dir&&strcmp(files[i].path,GALLERY_STORE))++expected_opens;CHECK(opens==expected_opens);
 unsigned before=opens;job.first=1197;page(&job);CHECK(result.count==7&&!result.more&&opens==before);
 job.first=9999;page(&job);CHECK(result.index==1197&&result.count==7);
 gallery_job_t image=job;job.first=1202;memset(&result,0,sizeof(result));CHECK(gallery_catalog_picture(&job,&result,&image));CHECK(image.path[0]&&opens==before);
 ++rev;page(&job);CHECK(opens>before&&!result.error_code);before=opens;++card;page(&job);CHECK(opens>before);
 job.first=0;job.recursive=false;job.kind=GALLERY_ALBUMS;page(&job);CHECK(result.directories==5&&result.total==5&&result.entries[0].directory);
 bool root_album=false,picture_album=false,trip_album=false,hidden_album=false,deep_album=false;
 for(unsigned i=0;i<result.count;++i){gallery_entry_t*e=&result.entries[i];CHECK(e->cover[0]&&image_name(e->cover));CHECK(!strcmp(e->name,strrchr(e->path,'/')+1));CHECK(strncmp(e->cover,e->path,strlen(e->path))==0&&e->cover[strlen(e->path)]=='/');CHECK(!strchr(e->cover+strlen(e->path)+1,'/'));CHECK(strcmp(e->path,"/sdcard/Notes")&&strcmp(e->path,"/sdcard/.epd")&&strcmp(e->path,"/sdcard/System Volume Information")&&strcmp(e->path,"/sdcard/nest"));for(unsigned j=0;j<i;++j)CHECK(strcmp(e->path,result.entries[j].path));root_album|=!strcmp(e->path,"/sdcard");picture_album|=!strcmp(e->path,"/sdcard/picture");trip_album|=!strcmp(e->path,"/sdcard/picture/Trip");hidden_album|=!strcmp(e->path,"/sdcard/.hidden");deep_album|=!strcmp(e->cover,deep);}
 CHECK(root_album&&picture_album&&trip_album&&hidden_album&&deep_album);
 memset(&result,0,sizeof(result));CHECK(!gallery_catalog_picture(&job,&result,&image)&&result.error_code==GALLERY_STATUS_UNAVAILABLE);
 job.kind=GALLERY_SCAN;strcpy(job.path,"/sdcard/picture");page(&job);CHECK(!result.directories&&result.total==1200&&!result.entries[0].directory);
 strcpy(job.path,"/sdcard/picture/Trip");page(&job);CHECK(result.total==1&&!strcmp(result.entries[0].path,"/sdcard/picture/Trip/new.JPEG"));
 strcpy(job.path,"/sdcard/picture");
 CHECK(peak_bytes<4096&&peak_fds<=2);gallery_catalog_clear();CHECK(!live&&!open_fds&&!locks);
 cancel_at=reads+2;snapshot(&job);while(gallery_catalog_step()){}CHECK(valid&&!scanning&&!locks);cancel_at=0;page(&job);CHECK(result.total==1200);gallery_catalog_clear();CHECK(!live&&!open_fds);
 remove_at=reads+2;page(&job);gallery_catalog_step();CHECK(!valid&&!live&&!open_fds&&!locks);remove_at=0;present=true;++card;
 writable=false;fail_at=allocations+1;page(&job);CHECK(result.error_code==GALLERY_STATUS_MEMORY&&!live&&!open_fds);fail_at=0;gallery_catalog_clear();writable=true;
 writable=false;job.recursive=true;strcpy(job.path,"/sdcard");page(&job);CHECK(result.total==1204&&!result.error_code&&live==1&&!open_fds);job.first=99999;page(&job);CHECK(result.count==7&&result.index==1197);gallery_catalog_clear();writable=true;
 writable=false;job.kind=GALLERY_ALBUMS;job.recursive=false;job.first=0;page(&job);CHECK(result.total==5&&result.directories==5&&result.complete&&!result.error_code);gallery_catalog_clear();writable=true;job.kind=GALLERY_SCAN;job.recursive=true;
 strcpy(job.path,"/flash");page(&job);CHECK(result.error_code==GALLERY_STATUS_PATH);
 strcpy(job.path,"/sdcard");present=false;page(&job);CHECK(result.error_code==GALLERY_STATUS_STORAGE);present=true;
 gallery_catalog_clear();fail_directory="/sdcard";snapshot(&job);CHECK(result.error_code==GALLERY_STATUS_SCAN&&!result.complete);gallery_catalog_clear();fail_directory=NULL;
 readdir_error_at=reads+1;snapshot(&job);CHECK(result.error_code==GALLERY_STATUS_SCAN&&!result.complete);gallery_catalog_clear();readdir_error_at=0;
 // Cache persists across catalog/service lifetimes and validates every source byte.
 uint32_t initial_revision=gallery_storage_revision();
 gallery_job_t photo={.serial=1,.kind=GALLERY_THUMB};strcpy(photo.path,"/sdcard/root.jpg");
 for(unsigned mode=0;mode<2;++mode){
  photo.kind=mode?GALLERY_IMAGE:GALLERY_THUMB;gallery_result_t a={0},b={0};gallery_decoder_t d={.fd=-1};
  CHECK(gallery_cached_render(&photo,&d,&a));CHECK(a.image.header.w==(mode?684:184));epd_app_free(d.gray);
  unsigned n=decodes;d=(gallery_decoder_t){.fd=-1};CHECK(gallery_cached_render(&photo,&d,&b));CHECK(decodes==n&&a.image.data_size==b.image.data_size&&!memcmp(a.image.data,b.image.data,a.image.data_size));
  release(&a);release(&b);char path[512];cache_path(&photo,path,512);file_t*f=find_file(path);CHECK(f);f->data[f->size-1]^=1;
  d=(gallery_decoder_t){.fd=-1};CHECK(gallery_cached_render(&photo,&d,&a)&&decodes==n+1);release(&a);epd_app_free(d.gray);
 }
 file_t*src=find_file(photo.path);src->data[0]=33;
 gallery_decoder_t d={.fd=-1};gallery_result_t r={0};unsigned n=decodes;CHECK(gallery_cached_render(&photo,&d,&r)&&decodes==n+1);release(&r);epd_app_free(d.gray);
 src->data[0]=34;d=(gallery_decoder_t){.fd=-1};CHECK(gallery_cached_render(&photo,&d,&r)&&decodes==n+2);release(&r);epd_app_free(d.gray);
 // A partial write is a miss; cancelled reads and card replacement free their allocation.
 src->data[0]=35;write_fail=io_count+1;d=(gallery_decoder_t){.fd=-1};CHECK(gallery_cached_render(&photo,&d,&r));release(&r);epd_app_free(d.gray);write_fail=0;
 d=(gallery_decoder_t){.fd=-1};n=decodes;CHECK(gallery_cached_render(&photo,&d,&r)&&decodes==n+1);release(&r);epd_app_free(d.gray);
 cancel_io=io_count+2;d=(gallery_decoder_t){.fd=-1};CHECK(!gallery_cached_render(&photo,&d,&r)&&!r.pixels);cancel_io=0;cancelled=false;
 remove_io=io_count+2;CHECK(!gallery_cached_render(&photo,&d,&r)&&!r.pixels&&r.error_code==GALLERY_STATUS_REMOVED);remove_io=0;present=true;
 CHECK(gallery_storage_revision()==initial_revision);++rev;CHECK(gallery_storage_revision()!=initial_revision);
 gallery_catalog_clear();CHECK(!live&&!open_fds&&!locks);
 // Fixed hash slots replace earlier entries rather than growing without bound.
 photo.kind=GALLERY_THUMB;
 for(unsigned i=0;i<140;++i){snprintf(photo.path,sizeof(photo.path),"/sdcard/picture/%04u.png",i);d=(gallery_decoder_t){.fd=-1};r=(gallery_result_t){0};CHECK(gallery_cached_render(&photo,&d,&r));release(&r);epd_app_free(d.gray);}
 unsigned thumbs=0,images=0;for(unsigned i=0;i<file_count;++i){if(!strncmp(files[i].path,GALLERY_STORE "/t",sizeof(GALLERY_STORE)+1))++thumbs;if(!strncmp(files[i].path,GALLERY_STORE "/i",sizeof(GALLERY_STORE)+1))++images;}
 CHECK(thumbs<=128&&images<=16&&!live&&!open_fds);
 // More than one page of image-owning folders, with no rescan on page two.
 for(unsigned i=0;i<12;++i){char folder[512],image_path[512];snprintf(folder,sizeof(folder),"/sdcard/Album%02u",i);add(folder,true,0);snprintf(image_path,sizeof(image_path),"%s/photo.JPG",folder);add(image_path,false,1);}++rev;
 job=(gallery_job_t){.serial=1,.kind=GALLERY_ALBUMS};strcpy(job.path,"/sdcard");page(&job);CHECK(result.total==17&&result.count==9&&result.more);before=opens;job.first=9;page(&job);CHECK(result.total==17&&result.count==8&&!result.more&&opens==before);gallery_catalog_clear();CHECK(!live&&!open_fds&&!locks);
 printf("gallery host: %u checks passed; catalog peak %zu bytes; 1204 images, media changes, read-only fallback, persistent cache integrity and short writes\n",checks,sizeof(walk_t));
 return 0;
}
