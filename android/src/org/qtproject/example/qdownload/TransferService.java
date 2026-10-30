package org.qtproject.example.qdownload;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;

/**
 * 传输期间的前台服务（M8）。
 *
 * 安卓在应用切到后台后会冻结普通进程，正在跑的 socket 传输会因此断掉；
 * 挂上前台服务 + 常驻通知后，系统才会把它当"用户知道并同意"的后台工作。
 *
 * 生命周期由 C++ 侧（src/app/AndroidService.cpp）按"有没有活跃传输"控制：
 * 有传输就 start，全部结束就 stop，不在后台白占一个通知位。
 */
public class TransferService extends Service
{
    private static final String CHANNEL_ID = "qdownload.foreground";
    private static final int NOTIFICATION_ID = 0x51444D;   // "QDM"

    @Override
    public int onStartCommand(Intent intent, int flags, int startId)
    {
        final String text = (intent != null)
                ? intent.getStringExtra("text")
                : null;

        createChannel();

        Notification.Builder builder;
        if (Build.VERSION.SDK_INT >= 26)
            builder = new Notification.Builder(this, CHANNEL_ID);
        else
            builder = new Notification.Builder(this);

        builder.setContentTitle("qdownload")
               .setContentText(text != null ? text : "正在传输文件")
               .setSmallIcon(android.R.drawable.stat_sys_download)
               .setOngoing(true);

        startForeground(NOTIFICATION_ID, builder.build());

        // 被系统杀掉后不要自己重启：传输状态在磁盘上，用户重新打开即可续传
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent)
    {
        return null;
    }

    private void createChannel()
    {
        if (Build.VERSION.SDK_INT < 26)
            return;

        NotificationManager manager =
                (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (manager == null)
            return;

        NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID, "文件传输", NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("传输进行中的常驻提示");
        manager.createNotificationChannel(channel);
    }
}
