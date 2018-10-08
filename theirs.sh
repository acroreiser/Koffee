xargs -L1 git checkout --theirs < theirs.txt
xargs -L1 git add < theirs.txt

HARD_RESET="drivers/gpu/drm include/drm drivers/devfreq include/linux/devfreq.h drivers/net/wireless/ath security/smack"

echo $HARD_RESET | xargs -L1 rm -fr
echo $HARD_RESET | xargs -L1 git checkout --theirs
echo $HARD_RESET | xargs -L1 git add
