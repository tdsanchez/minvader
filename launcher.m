#import <Cocoa/Cocoa.h>

#ifndef NSWindowStyleMaskTitled
#define NSWindowStyleMaskTitled NSTitledWindowMask
#define NSWindowStyleMaskClosable NSClosableWindowMask
#define NSWindowStyleMaskMiniaturizable NSMiniaturizableWindowMask
#endif
#ifndef NSControlStateValueOn
#define NSControlStateValueOn NSOnState
#endif
#ifndef NSBezelStyleRounded
#define NSBezelStyleRounded NSRoundedBezelStyle
#endif
#ifndef NSModalResponseOK
#define NSModalResponseOK 1
#endif

static NSTextField *portField;
static NSTextField *dirLabel;
static NSButton *warmCheck;
static NSButton *browseBtn;
static NSButton *startBtn;
static NSButton *stopBtn;
static NSTextField *statusLabel;
static NSString *chosenDir;
static NSTask *serverTask;
static NSPipe *serverPipe;

static NSString *coreBinaryPath(void) {
    NSString *bundle = [[NSBundle mainBundle] bundlePath];
    NSString *dir = [bundle stringByDeletingLastPathComponent];
    return [dir stringByAppendingPathComponent:@"minvader"];
}

@interface LauncherDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property (strong) NSWindow *window;
@end

@implementation LauncherDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)n {
    NSRect frame = NSMakeRect(200, 200, 480, 260);
    self.window = [[NSWindow alloc]
        initWithContentRect:frame
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered
        defer:NO];
    [self.window setTitle:@"minvader"];
    [self.window setDelegate:self];

    NSView *v = [self.window contentView];

    NSTextField *portLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 200, 50, 24)];
    [portLabel setStringValue:@"Port:"];
    [portLabel setBezeled:NO];
    [portLabel setDrawsBackground:NO];
    [portLabel setEditable:NO];
    [portLabel setSelectable:NO];
    [v addSubview:portLabel];

    portField = [[NSTextField alloc] initWithFrame:NSMakeRect(75, 200, 80, 24)];
    [portField setStringValue:@"9090"];
    [v addSubview:portField];

    warmCheck = [[NSButton alloc] initWithFrame:NSMakeRect(175, 200, 280, 24)];
    [warmCheck setButtonType:NSSwitchButton];
    [warmCheck setTitle:@"Warm start (use existing cache)"];
    [warmCheck setTarget:self];
    [warmCheck setAction:@selector(warmToggled:)];
    [v addSubview:warmCheck];

    NSTextField *dirTitleLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 160, 70, 24)];
    [dirTitleLabel setStringValue:@"Directory:"];
    [dirTitleLabel setBezeled:NO];
    [dirTitleLabel setDrawsBackground:NO];
    [dirTitleLabel setEditable:NO];
    [dirTitleLabel setSelectable:NO];
    [v addSubview:dirTitleLabel];

    dirLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(95, 160, 270, 24)];
    [dirLabel setStringValue:@"(none selected)"];
    [dirLabel setBezeled:NO];
    [dirLabel setDrawsBackground:NO];
    [dirLabel setEditable:NO];
    [dirLabel setSelectable:NO];
    [dirLabel setLineBreakMode:NSLineBreakByTruncatingMiddle];
    [v addSubview:dirLabel];

    browseBtn = [[NSButton alloc] initWithFrame:NSMakeRect(375, 158, 85, 28)];
    [browseBtn setTitle:@"Browse…"];
    [browseBtn setBezelStyle:NSBezelStyleRounded];
    [browseBtn setTarget:self];
    [browseBtn setAction:@selector(browse:)];
    [v addSubview:browseBtn];

    startBtn = [[NSButton alloc] initWithFrame:NSMakeRect(140, 110, 90, 32)];
    [startBtn setTitle:@"Start"];
    [startBtn setBezelStyle:NSBezelStyleRounded];
    [startBtn setTarget:self];
    [startBtn setAction:@selector(start:)];
    [v addSubview:startBtn];

    stopBtn = [[NSButton alloc] initWithFrame:NSMakeRect(250, 110, 90, 32)];
    [stopBtn setTitle:@"Stop"];
    [stopBtn setBezelStyle:NSBezelStyleRounded];
    [stopBtn setTarget:self];
    [stopBtn setAction:@selector(stop:)];
    [stopBtn setEnabled:NO];
    [v addSubview:stopBtn];

    statusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 60, 440, 40)];
    [statusLabel setStringValue:@"Ready"];
    [statusLabel setBezeled:NO];
    [statusLabel setDrawsBackground:NO];
    [statusLabel setEditable:NO];
    [statusLabel setSelectable:NO];
    [statusLabel setLineBreakMode:NSLineBreakByWordWrapping];
    [v addSubview:statusLabel];

    [self.window makeKeyAndOrderFront:nil];
}

- (void)warmToggled:(id)sender {
    BOOL warm = ([warmCheck state] == NSControlStateValueOn);
    [browseBtn setEnabled:!warm];
    if (warm) {
        [dirLabel setStringValue:@"(using cache DB)"];
    } else {
        [dirLabel setStringValue:chosenDir ? chosenDir : @"(none selected)"];
    }
}

- (void)browse:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setCanChooseDirectories:YES];
    [panel setCanChooseFiles:NO];
    [panel setAllowsMultipleSelection:NO];
    [panel setMessage:@"Choose a directory to serve"];
    if ([panel runModal] == NSModalResponseOK) {
        chosenDir = [[panel URL] path];
        [dirLabel setStringValue:chosenDir];
    }
}

- (void)start:(id)sender {
    NSString *port = [portField stringValue];
    if ([port length] == 0) {
        [statusLabel setStringValue:@"Error: port is required"];
        return;
    }
    BOOL warm = ([warmCheck state] == NSControlStateValueOn);
    if (!warm && !chosenDir) {
        [statusLabel setStringValue:@"Error: choose a directory or enable warm start"];
        return;
    }

    NSString *binary = coreBinaryPath();
    if (![[NSFileManager defaultManager] isExecutableFileAtPath:binary]) {
        [statusLabel setStringValue:[NSString stringWithFormat:@"Error: %@ not found", binary]];
        return;
    }

    serverTask = [[NSTask alloc] init];

    if (warm) {
        [serverTask setLaunchPath:binary];
        [serverTask setArguments:@[@"--warm", [NSString stringWithFormat:@"--port=%@", port]]];
    } else {
        [serverTask setLaunchPath:@"/bin/bash"];
        [serverTask setArguments:@[
            @"-c",
            [NSString stringWithFormat:@"find '%@' -type f | '%@' --stdin --port=%@",
                chosenDir, binary, port]
        ]];
    }

    serverPipe = [NSPipe pipe];
    [serverTask setStandardError:serverPipe];
    [serverTask setStandardOutput:serverPipe];

    __weak LauncherDelegate *weakSelf = self;
    [serverTask setTerminationHandler:^(NSTask *t) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf serverStopped];
        });
    }];

    NSFileHandle *fh = [serverPipe fileHandleForReading];
    [fh setReadabilityHandler:^(NSFileHandle *handle) {
        NSData *data = [handle availableData];
        if ([data length] > 0) {
            NSString *str = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
            if (str) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    NSString *last = [[str componentsSeparatedByString:@"\n"] lastObject];
                    if ([last length] == 0) {
                        NSArray *lines = [str componentsSeparatedByString:@"\n"];
                        if ([lines count] > 1) last = lines[[lines count] - 2];
                    }
                    if ([last length] > 0) [statusLabel setStringValue:last];
                });
            }
        }
    }];

    @try {
        [serverTask launch];
    } @catch (NSException *e) {
        [statusLabel setStringValue:[NSString stringWithFormat:@"Launch failed: %@", [e reason]]];
        return;
    }

    [statusLabel setStringValue:[NSString stringWithFormat:@"Starting on port %@…", port]];
    [startBtn setEnabled:NO];
    [stopBtn setEnabled:YES];
    [portField setEnabled:NO];
    [warmCheck setEnabled:NO];
    [browseBtn setEnabled:NO];
}

- (void)stop:(id)sender {
    if (serverTask && [serverTask isRunning]) {
        [serverTask terminate];
    }
}

- (void)serverStopped {
    [statusLabel setStringValue:@"Server stopped"];
    [startBtn setEnabled:YES];
    [stopBtn setEnabled:NO];
    [portField setEnabled:YES];
    [warmCheck setEnabled:YES];
    BOOL warm = ([warmCheck state] == NSControlStateValueOn);
    [browseBtn setEnabled:!warm];
    serverTask = nil;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {
    return YES;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    if (serverTask && [serverTask isRunning]) {
        [serverTask terminate];
        [serverTask waitUntilExit];
    }
    return NSTerminateNow;
}

- (BOOL)windowShouldClose:(NSWindow *)w {
    if (serverTask && [serverTask isRunning]) {
        [serverTask terminate];
        [serverTask waitUntilExit];
    }
    [NSApp terminate:nil];
    return YES;
}

@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        NSMenu *menubar = [[NSMenu alloc] init];
        NSMenuItem *appMenuItem = [[NSMenuItem alloc] init];
        [menubar addItem:appMenuItem];
        NSMenu *appMenu = [[NSMenu alloc] init];
        [appMenu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
        [appMenuItem setSubmenu:appMenu];
        [app setMainMenu:menubar];

        LauncherDelegate *del = [[LauncherDelegate alloc] init];
        [app setDelegate:del];
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
